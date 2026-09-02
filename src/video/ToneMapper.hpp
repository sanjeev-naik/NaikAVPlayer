#pragma once

// HDR -> SDR tone mapping for the video conversion path.
//
// Why this exists: the decoder's conversion step targets 8-bit YUV420P,
// which is what the SDL streaming texture consumes. Handing a PQ- or
// HLG-encoded 10-bit frame straight to sws_scale gets it *resampled* to
// 8 bits but leaves the transfer curve untouched -- the numbers are still
// PQ code values, and a display interpreting them as plain BT.709 gamma
// renders the picture dark and desaturated. Bit depth was never the
// problem; the missing step is the conversion from the HDR transfer
// function and BT.2020 gamut into SDR display light.
//
// Structure mirrors the audio DSP modules: header-only, no state shared
// with the rest of the pipeline, and a true no-op unless the source
// actually carries an HDR transfer characteristic.
//
// Pipeline per pixel (see process()):
//
//   PQ/HLG code -> display linear (nits, BT.2020)
//        -> BT.2390 EETF roll-off toward the SDR peak
//        -> BT.2020 -> BT.709 gamut conversion (linear light)
//        -> soft desaturation of out-of-gamut values
//        -> BT.709 OETF -> 8-bit
//
// The transcendental parts (both EOTFs, the tone curve, the OETF) are all
// scalar functions of one value, so each is precomputed into a lookup
// table once per (transfer, source peak, target peak) combination. The
// per-pixel path is then table reads, one sqrt, and a 3x3 matrix.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

// The per-pixel path is called millions of times per frame, and both
// compilers decline to inline it on size grounds when left to their own
// judgement -- which turns every pixel into a real call and costs more
// than the arithmetic inside it.
#if defined(_MSC_VER)
#define NAIKAV_TM_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define NAIKAV_TM_INLINE inline __attribute__((always_inline))
#else
#define NAIKAV_TM_INLINE inline
#endif

namespace naikav {
namespace video {

// Which HDR transfer function the source carries. None means SDR -- the
// tone mapper stays entirely out of the way.
enum class HdrTransfer {
    None = 0,
    PQ,   // SMPTE ST 2084 (HDR10, HDR10+, Dolby Vision base layer)
    HLG   // ARIB STD-B67 (Hybrid Log-Gamma)
};

struct HdrToneMapSettings {
    // Master switch. When false the decoder leaves HDR frames on the old
    // straight-to-8-bit path, which is the pre-tone-mapping behavior --
    // kept reachable so the change is verifiable side by side.
    bool enabled = true;

    // Peak luminance of the display the picture is being mapped onto.
    // 100 nits is the SDR reference white / diffuse peak assumed by
    // BT.1886 and by every non-HDR monitor.
    float targetPeakNits = 100.0f;

    // Peak luminance the content was mastered for. 0 means "derive it":
    // the mastering-display metadata when the file carries it, otherwise
    // kDefaultSourcePeakNits below.
    float sourcePeakNits = 0.0f;
};

// Fallback mastering peak when a file carries no mastering-display
// metadata. 1000 nits is the near-universal grading target for consumer
// HDR10, and is also HLG's nominal system reference peak.
inline constexpr float kDefaultSourcePeakNits = 1000.0f;

// ---------------------------------------------------------------------
// Transfer functions
// ---------------------------------------------------------------------

// SMPTE ST 2084 constants. Expressed as the spec's exact rationals rather
// than rounded decimals so the inverse below round-trips cleanly.
inline constexpr double kPqM1 = 2610.0 / 16384.0;
inline constexpr double kPqM2 = 2523.0 / 4096.0 * 128.0;
inline constexpr double kPqC1 = 3424.0 / 4096.0;
inline constexpr double kPqC2 = 2413.0 / 4096.0 * 32.0;
inline constexpr double kPqC3 = 2392.0 / 4096.0 * 32.0;

// PQ EOTF: non-linear code [0,1] -> linear light normalized so 1.0 is the
// PQ system peak of 10000 nits.
inline double pqEotf(double e) {
    if (e <= 0.0) return 0.0;
    if (e >= 1.0) return 1.0;
    const double p = std::pow(e, 1.0 / kPqM2);
    const double num = std::max(p - kPqC1, 0.0);
    const double den = kPqC2 - kPqC3 * p;
    if (den <= 0.0) return 1.0;
    return std::pow(num / den, 1.0 / kPqM1);
}

// PQ inverse EOTF: linear light (1.0 == 10000 nits) -> non-linear code.
inline double pqInverseEotf(double y) {
    if (y <= 0.0) return 0.0;
    if (y >= 1.0) return 1.0;
    const double yp = std::pow(y, kPqM1);
    return std::pow((kPqC1 + kPqC2 * yp) / (1.0 + kPqC3 * yp), kPqM2);
}

// ARIB STD-B67 (HLG) constants.
inline constexpr double kHlgA = 0.17883277;
inline constexpr double kHlgB = 0.28466892;  // 1 - 4a
inline constexpr double kHlgC = 0.55991073;  // 0.5 - a*ln(4a)

// HLG inverse OETF: signal [0,1] -> *scene* linear [0,1].
//
// Note this is only half of HLG's decode. The other half -- the OOTF that
// turns scene light into display light -- depends on all three channels
// at once (it is driven by the pixel's luminance), so it cannot live in a
// per-channel table and is applied in process() instead.
inline double hlgInverseOetf(double e) {
    if (e <= 0.0) return 0.0;
    if (e <= 0.5) return (e * e) / 3.0;
    return (std::exp((e - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

// HLG's OOTF exponent for a display of peak Lw, per ITU-R BT.2100.
inline double hlgSystemGamma(double peakNits) {
    if (peakNits <= 0.0) return 1.2;
    return 1.2 + 0.42 * std::log10(peakNits / 1000.0);
}

// BT.709 OETF -- the encode side of an SDR video signal. Applied last, so
// the 8-bit values handed downstream mean what a BT.709-tagged frame is
// supposed to mean.
inline double bt709Oetf(double l) {
    if (l <= 0.0) return 0.0;
    if (l >= 1.0) return 1.0;
    if (l < 0.018) return 4.5 * l;
    return 1.099 * std::pow(l, 0.45) - 0.099;
}

// ---------------------------------------------------------------------
// BT.2390 EETF
// ---------------------------------------------------------------------

// ITU-R BT.2390 electro-electrical transfer function: the reference HDR
// roll-off. It runs in PQ space (perceptually uniform, which is what
// makes a single Hermite knee behave well across the whole range) and
// leaves everything below the knee untouched, so SDR-range detail passes
// through unchanged and only the highlights are compressed.
//
// srcPeakNits / dstPeakNits are absolute luminances; the return value is
// in nits, clamped to dstPeakNits.
inline double bt2390Eetf(double lumaNits, double srcPeakNits, double dstPeakNits) {
    if (lumaNits <= 0.0) return 0.0;

    const double pqSrcPeak = pqInverseEotf(srcPeakNits / 10000.0);
    if (pqSrcPeak <= 0.0) return 0.0;

    // Normalize into the source's PQ range. Black level (minLum) is taken
    // as 0, which drops the spec's black-lift term -- correct for the
    // display-referred case here, where nothing lifts the floor.
    const double e1 = pqInverseEotf(std::min(lumaNits, srcPeakNits) / 10000.0) / pqSrcPeak;
    const double maxLum = pqInverseEotf(dstPeakNits / 10000.0) / pqSrcPeak;

    // Target peak at or above source peak: nothing to roll off.
    if (maxLum >= 1.0) {
        return std::min(lumaNits, dstPeakNits);
    }

    // Knee start. Clamped below 1.0 so the spline denominator stays finite.
    const double ks = std::min(1.5 * maxLum - 0.5, 0.9999);

    double e2 = e1;
    if (e1 > ks) {
        const double t = (e1 - ks) / (1.0 - ks);
        const double t2 = t * t;
        const double t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks +
             (t3 - 2.0 * t2 + t) * (1.0 - ks) +
             (-2.0 * t3 + 3.0 * t2) * maxLum;
    }

    const double out = pqEotf(e2 * pqSrcPeak) * 10000.0;
    return std::min(out, dstPeakNits);
}

// ---------------------------------------------------------------------
// ToneMapper
// ---------------------------------------------------------------------

class ToneMapper {
public:
    // Table sizes. The EOTF table is indexed by the top 12 bits of the
    // 16-bit input with interpolation across the remainder: 12-bit PQ is
    // the professional mastering depth, so this is finer than any source
    // that reaches it, and the interpolation keeps the shadow ramp (where
    // PQ is steepest) free of banding.
    static constexpr int kEotfLutBits = 12;
    static constexpr int kEotfLutSize = 1 << kEotfLutBits;   // 4096
    static constexpr int kEotfLutShift = 16 - kEotfLutBits;  // 4

    // The tone-curve table shares the EOTF table's size, but not always
    // its domain -- see m_gain below.
    static constexpr int kGainLutSize = kEotfLutSize;

    // Output table maps normalized display light straight to 8 bits. At
    // 4096 entries it oversamples the 8-bit output 16x, so no
    // interpolation is needed on this one.
    static constexpr int kOetfLutSize = 4096;

    ToneMapper() = default;

    // Build (or reuse) the tables for this combination. Returns false for
    // an SDR source, in which case the mapper must not be used.
    //
    // Rebuilding is ~10k transcendental evaluations, so it is guarded on
    // the parameters actually changing -- in practice it runs once per
    // file, not per frame.
    bool configure(HdrTransfer transfer, float srcPeakNits, float dstPeakNits) {
        if (transfer == HdrTransfer::None) {
            m_ready = false;
            return false;
        }
        if (srcPeakNits <= 0.0f) srcPeakNits = kDefaultSourcePeakNits;
        if (dstPeakNits <= 0.0f) dstPeakNits = 100.0f;

        // A source that claims a peak at or below the target still needs
        // the transfer and gamut conversion, but the roll-off would be a
        // no-op knee. Keep the peak strictly above the target so the
        // curve stays well-conditioned.
        srcPeakNits = std::max(srcPeakNits, dstPeakNits * 1.0001f);

        if (m_ready && transfer == m_transfer &&
            srcPeakNits == m_srcPeakNits && dstPeakNits == m_dstPeakNits) {
            return true;
        }

        m_transfer = transfer;
        m_srcPeakNits = srcPeakNits;
        m_dstPeakNits = dstPeakNits;
        buildTables();
        m_ready = true;
        return true;
    }

    bool isReady() const { return m_ready; }
    HdrTransfer transfer() const { return m_transfer; }
    float sourcePeakNits() const { return m_srcPeakNits; }
    float targetPeakNits() const { return m_dstPeakNits; }

    // Convert a packed 16-bit-per-channel BT.2020 HDR image into packed
    // 8-bit BT.709 SDR RGB.
    //
    // src: RGB48 in native endianness (AV_PIX_FMT_RGB48), srcStride in bytes.
    // dst: RGB24, dstStride in bytes.
    //
    // Rows are independent, so the work is split across hardware threads
    // for anything large enough to pay for the spawn. A 4K frame is ~8M
    // pixels of table lookups and a 3x3 matrix; single-threaded that is
    // the largest cost in the conversion step, and splitting it puts it
    // back under the frame budget.
    void process(const uint8_t* src, int srcStride,
                 uint8_t* dst, int dstStride,
                 int width, int height, int maxWorkers = 0) const {
        if (!m_ready || !src || !dst || width <= 0 || height <= 0) {
            return;
        }

        int workers = chooseWorkerCount(width, height);
        if (maxWorkers > 0) {
            workers = std::max(1, std::min(workers, maxWorkers));
        }
        if (workers <= 1) {
            processRows(src, srcStride, dst, dstStride, width, 0, height);
            return;
        }

        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(workers) - 1);
        const int rowsPer = (height + workers - 1) / workers;
        for (int w = 1; w < workers; ++w) {
            const int y0 = w * rowsPer;
            if (y0 >= height) break;
            const int y1 = std::min(y0 + rowsPer, height);
            pool.emplace_back([this, src, srcStride, dst, dstStride, width, y0, y1]() {
                processRows(src, srcStride, dst, dstStride, width, y0, y1);
            });
        }
        // The calling thread takes the first slice rather than idling.
        processRows(src, srcStride, dst, dstStride, width, 0,
                    std::min(rowsPer, height));
        for (auto& t : pool) {
            t.join();
        }
    }

    // Exposed for tests: the full scalar path for one pixel, in the same
    // order and with the same tables process() uses.
    void mapPixel(uint16_t r16, uint16_t g16, uint16_t b16,
                  uint8_t& r8, uint8_t& g8, uint8_t& b8) const {
        if (!m_ready) {
            r8 = g8 = b8 = 0;
            return;
        }
        mapPixelImpl(r16, g16, b16, r8, g8, b8);
    }

private:
    bool m_ready = false;
    HdrTransfer m_transfer = HdrTransfer::None;
    float m_srcPeakNits = kDefaultSourcePeakNits;
    float m_dstPeakNits = 100.0f;

    // Every float table carries one guard entry past the end so the
    // interpolating lookups can read [idx] and [idx+1] unconditionally --
    // no bounds branch in the inner loop.

    // code -> linear light. For PQ this is display light in nits; for HLG
    // it is scene light in [0,1], with the OOTF applied per pixel.
    std::array<float, kEotfLutSize + 1> m_eotf{};
    // HLG only: scene luma -> the OOTF's Lw * Ys^(gamma-1) scale factor.
    std::array<float, kGainLutSize + 1> m_ootf{};
    // Tone-map gain, with the 1/targetPeak normalization folded in so the
    // result lands in [0,1]. Its domain depends on the transfer function:
    //
    //   PQ  -- indexed by the source *code value*, exactly like m_eotf.
    //          PQ is already perceptually uniform, and the brightest
    //          channel's code is available before any decoding, so this
    //          costs one gather and needs no luminance normalization.
    //   HLG -- indexed linearly by display nits / sourcePeak, because the
    //          OOTF makes display light depend on all three channels, so
    //          a single channel's code no longer determines it.
    std::array<float, kGainLutSize + 1> m_gain{};
    bool m_gainIndexedByCode = true;
    float m_gainNitsScale = 0.0f;  // HLG: (kGainLutSize-1) / sourcePeak
    // normalized display light -> 8-bit BT.709 code.
    std::array<uint8_t, kOetfLutSize> m_oetf{};

    void buildTables() {
        const double srcPeak = m_srcPeakNits;
        const double dstPeak = m_dstPeakNits;

        // --- EOTF ---
        for (int i = 0; i < kEotfLutSize; ++i) {
            const double e = static_cast<double>(i) / (kEotfLutSize - 1);
            if (m_transfer == HdrTransfer::PQ) {
                m_eotf[i] = static_cast<float>(pqEotf(e) * 10000.0);
            } else {
                m_eotf[i] = static_cast<float>(hlgInverseOetf(e));
            }
        }
        m_eotf[kEotfLutSize] = m_eotf[kEotfLutSize - 1];  // guard entry

        // --- Tone curve ---
        // The stored value is the gain taking linear source light to
        // normalized display light, so the per-pixel path multiplies once
        // instead of dividing.
        m_gainIndexedByCode = (m_transfer == HdrTransfer::PQ);
        m_gainNitsScale =
            m_gainIndexedByCode ? 0.0f
                                : static_cast<float>((kGainLutSize - 1) / srcPeak);
        for (int i = 0; i < kGainLutSize; ++i) {
            const double t = static_cast<double>(i) / (kGainLutSize - 1);
            // PQ: t is a code value, so decode it. HLG: t is already a
            // fraction of the source peak.
            const double lNits =
                m_gainIndexedByCode ? pqEotf(t) * 10000.0 : t * srcPeak;
            if (lNits <= 0.0) {
                m_gain[i] = 0.0f;  // patched below
                continue;
            }
            const double mapped = bt2390Eetf(lNits, srcPeak, dstPeak);
            m_gain[i] = static_cast<float>((mapped / lNits) / dstPeak);
        }
        // Below the knee the EETF is the identity, so the gain tends to
        // 1/dstPeak as luminance tends to zero. Reuse the first real
        // entry rather than dividing by zero at the origin.
        m_gain[0] = (kGainLutSize > 1) ? m_gain[1] : static_cast<float>(1.0 / dstPeak);
        m_gain[kGainLutSize] = m_gain[kGainLutSize - 1];  // guard entry

        // --- HLG OOTF ---
        if (m_transfer == HdrTransfer::HLG) {
            const double gamma = hlgSystemGamma(srcPeak);
            for (int i = 0; i < kGainLutSize; ++i) {
                const double ys = static_cast<double>(i) / (kGainLutSize - 1);
                m_ootf[i] = static_cast<float>(srcPeak * std::pow(ys, gamma - 1.0));
            }
            // Ys == 0 leaves the exponent undefined for gamma > 1; the
            // limit is 0 and the pixel is black either way.
            m_ootf[0] = 0.0f;
            m_ootf[kGainLutSize] = m_ootf[kGainLutSize - 1];
        } else {
            m_ootf.fill(0.0f);
        }

        // --- OETF ---
        for (int i = 0; i < kOetfLutSize; ++i) {
            const double l = static_cast<double>(i) / (kOetfLutSize - 1);
            const double e = bt709Oetf(l);
            int v = static_cast<int>(std::lround(e * 255.0));
            m_oetf[i] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }

    static int chooseWorkerCount(int width, int height) {
        // Spawning the workers costs a few hundred microseconds all told,
        // against roughly 30ns per pixel of work. Past ~65k pixels the
        // work dominates by an order of magnitude, so anything from about
        // 320x200 up is worth splitting; below that it is not.
        const long long pixels = static_cast<long long>(width) * height;
        if (pixels < (1 << 16)) return 1;
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        int workers = static_cast<int>(std::min<unsigned>(hw, 8u));
        return std::max(1, std::min(workers, height));
    }

    // Interpolated lookup keyed by a full 16-bit code. Branch-free: the
    // tables carry a guard entry so [idx+1] is always valid.
    template <size_t N>
    NAIKAV_TM_INLINE static float codeLookup(const std::array<float, N>& lut,
                                             uint16_t code) {
        const int idx = code >> kEotfLutShift;
        const float f = static_cast<float>(code & ((1 << kEotfLutShift) - 1)) *
                        (1.0f / (1 << kEotfLutShift));
        const float a = lut[idx];
        return a + (lut[idx + 1] - a) * f;
    }

    // Interpolated lookup into a [0,1]-domain table, given a position
    // already scaled into table units.
    template <size_t N>
    NAIKAV_TM_INLINE static float posLookup(const std::array<float, N>& lut,
                                            float pos) {
        pos = std::clamp(pos, 0.0f, static_cast<float>(N - 2));
        const int idx = static_cast<int>(pos);
        const float f = pos - static_cast<float>(idx);
        const float a = lut[idx];
        return a + (lut[idx + 1] - a) * f;
    }

    // BT.2020 -> BT.709 in linear light, written as identity-plus-delta
    // rather than as a plain 3x3 product.
    //
    // Both gamuts are normalized to the same D65 white, so every row of
    // the true matrix sums to exactly 1 and a neutral grey must come out
    // unchanged. The published coefficients are rounded to six decimals
    // and row 2 sums to 1.000001, which is enough to shift greys off
    // neutral by a code value at some luminances. Expressing each row as
    // "keep this channel, then correct by the differences" makes the
    // diagonal term implicit, so equal inputs give equal outputs exactly,
    // whatever the rounding.
    NAIKAV_TM_INLINE static void bt2020ToBt709(float r, float g, float b,
                                               float& r7, float& g7, float& b7) {
        r7 = r + (-0.587641f) * (g - r) + (-0.072850f) * (b - r);
        g7 = g + (-0.124550f) * (r - g) + (-0.008349f) * (b - g);
        b7 = b + (-0.018151f) * (r - b) + (-0.100579f) * (g - b);
    }

    NAIKAV_TM_INLINE void mapPixelImpl(uint16_t r16, uint16_t g16, uint16_t b16,
                                       uint8_t& r8, uint8_t& g8, uint8_t& b8) const {
        // 1. Decode the transfer function into linear light.
        float r = codeLookup(m_eotf, r16);
        float g = codeLookup(m_eotf, g16);
        float b = codeLookup(m_eotf, b16);

        // 2. Tone map. The gain is driven by the brightest channel, so
        //    the result can never exceed the target peak, and applying
        //    one gain to all three channels holds the hue steady instead
        //    of shifting it the way per-channel compression does.
        float gain;
        if (m_gainIndexedByCode) {
            // PQ. The code values are monotonic in luminance, so the
            // brightest channel can be picked before decoding and used to
            // index the curve directly -- no normalization, no division.
            const uint16_t maxCode = std::max(r16, std::max(g16, b16));
            gain = codeLookup(m_gain, maxCode);
        } else {
            // HLG. The OOTF turns scene light into display light using
            // the pixel's own luminance, so it has to run before the tone
            // curve and it cannot be folded into a per-channel table.
            const float ys = 0.2627f * r + 0.6780f * g + 0.0593f * b;
            const float scale = posLookup(m_ootf, ys * (kGainLutSize - 1));
            r *= scale;
            g *= scale;
            b *= scale;
            const float maxc = std::max(r, std::max(g, b));
            gain = posLookup(m_gain, maxc * m_gainNitsScale);
        }
        r *= gain;
        g *= gain;
        b *= gain;

        // 3. BT.2020 -> BT.709 primaries, in linear light (the only place
        //    a gamut conversion is meaningful).
        float r7, g7, b7;
        bt2020ToBt709(r, g, b, r7, g7, b7);

        // 4. BT.2020 covers colors BT.709 cannot represent, which land
        //    outside [0,1] here. Pulling them toward the pixel's own luma
        //    desaturates just enough to fit, which reads as a slightly
        //    duller color; hard clipping each channel independently would
        //    instead swing the hue.
        desaturateIntoRange(r7, g7, b7);

        // 5. Encode to 8-bit BT.709.
        r8 = m_oetf[toOetfIndex(r7)];
        g8 = m_oetf[toOetfIndex(g7)];
        b8 = m_oetf[toOetfIndex(b7)];
    }

    // Pull an out-of-gamut color toward its own luminance until every
    // channel fits in [0,1]. Deliberately branch-free: this sits in the
    // block loop below, and a data-dependent branch here stops the
    // compiler vectorizing the whole phase.
    //
    // Both shrink factors are >= 1 whenever that end is already in range
    // (with minc >= 0, luma - minc <= luma, so the ratio cannot go below
    // 1), so the unconditional min() against 1 picks whichever end is
    // actually binding and leaves in-gamut pixels untouched. A luma above
    // 1 drives sHi negative, s clamps to 0, and the pixel correctly
    // resolves to white.
    NAIKAV_TM_INLINE static void desaturateIntoRange(float& r, float& g, float& b) {
        constexpr float kEps = 1e-6f;
        const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        const float lc = std::max(luma, 0.0f);
        const float minc = std::min(r, std::min(g, b));
        const float maxc = std::max(r, std::max(g, b));

        const float sLo = std::min(lc / std::max(lc - minc, kEps), 1.0f);
        const float sHi = std::min((1.0f - lc) / std::max(maxc - lc, kEps), 1.0f);
        const float s = std::clamp(std::min(sLo, sHi), 0.0f, 1.0f);

        r = std::clamp(lc + (r - lc) * s, 0.0f, 1.0f);
        g = std::clamp(lc + (g - lc) * s, 0.0f, 1.0f);
        b = std::clamp(lc + (b - lc) * s, 0.0f, 1.0f);
    }

    NAIKAV_TM_INLINE static int toOetfIndex(float v) {
        const int i = static_cast<int>(std::clamp(v, 0.0f, 1.0f) * (kOetfLutSize - 1) + 0.5f);
        return std::clamp(i, 0, kOetfLutSize - 1);
    }

    // Pixels per staging block. Small enough that the three float arrays
    // stay in L1, large enough to amortize the phase boundaries.
    static constexpr int kBlock = 128;

    // Rows are processed in blocks split into three phases rather than
    // one pixel at a time, because the phases have very different shapes:
    // the table reads are scalar gathers that cannot vectorize, while the
    // gain/matrix/gamut arithmetic between them is straight-line float
    // math over contiguous arrays that vectorizes cleanly. Interleaved
    // per pixel, the gathers stall the arithmetic and the whole loop
    // compiles to scalar code; separated, only the gathers stay scalar.
    void processRows(const uint8_t* src, int srcStride,
                     uint8_t* dst, int dstStride,
                     int width, int y0, int y1) const {
        alignas(32) float lr[kBlock];
        alignas(32) float lg[kBlock];
        alignas(32) float lb[kBlock];
        alignas(32) float gn[kBlock];

        const bool byCode = m_gainIndexedByCode;

        for (int y = y0; y < y1; ++y) {
            // Native-endian RGB48 (AV_PIX_FMT_RGB48 resolves to the LE or
            // BE variant to match the host), so the row reads directly as
            // uint16_t. av_frame_get_buffer's alignment covers the cast.
            const uint16_t* rowSrc =
                reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(y) * srcStride);
            uint8_t* rowDst = dst + static_cast<size_t>(y) * dstStride;

            for (int x0 = 0; x0 < width; x0 += kBlock) {
                const int n = std::min(kBlock, width - x0);
                const uint16_t* s = rowSrc + static_cast<size_t>(x0) * 3;
                uint8_t* d = rowDst + static_cast<size_t>(x0) * 3;

                // Phase 1: table reads into planar form.
                for (int i = 0; i < n; ++i) {
                    const uint16_t cr = s[3 * i + 0];
                    const uint16_t cg = s[3 * i + 1];
                    const uint16_t cb = s[3 * i + 2];
                    lr[i] = codeLookup(m_eotf, cr);
                    lg[i] = codeLookup(m_eotf, cg);
                    lb[i] = codeLookup(m_eotf, cb);
                    if (byCode) {
                        gn[i] = codeLookup(m_gain, std::max(cr, std::max(cg, cb)));
                    }
                }

                // Phase 1b: HLG's OOTF, which needs the decoded values.
                if (!byCode) {
                    for (int i = 0; i < n; ++i) {
                        const float ys = 0.2627f * lr[i] + 0.6780f * lg[i] + 0.0593f * lb[i];
                        const float scale = posLookup(m_ootf, ys * (kGainLutSize - 1));
                        lr[i] *= scale;
                        lg[i] *= scale;
                        lb[i] *= scale;
                        const float maxc = std::max(lr[i], std::max(lg[i], lb[i]));
                        gn[i] = posLookup(m_gain, maxc * m_gainNitsScale);
                    }
                }

                // Phase 2: the vectorizable part -- tone-map gain, gamut
                // conversion, and the soft gamut clip, with no gathers
                // and no branches.
                for (int i = 0; i < n; ++i) {
                    const float r = lr[i] * gn[i];
                    const float g = lg[i] * gn[i];
                    const float b = lb[i] * gn[i];
                    float r7, g7, b7;
                    bt2020ToBt709(r, g, b, r7, g7, b7);
                    desaturateIntoRange(r7, g7, b7);
                    lr[i] = r7;
                    lg[i] = g7;
                    lb[i] = b7;
                }

                // Phase 3: encode, the second unavoidable gather.
                for (int i = 0; i < n; ++i) {
                    d[3 * i + 0] = m_oetf[toOetfIndex(lr[i])];
                    d[3 * i + 1] = m_oetf[toOetfIndex(lg[i])];
                    d[3 * i + 2] = m_oetf[toOetfIndex(lb[i])];
                }
            }
        }
    }
};

} // namespace video
} // namespace naikav
