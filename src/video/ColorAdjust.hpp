#pragma once

// Picture / colour adjustment for the video conversion path.
//
// Why this exists: every control the player had over the picture lived in
// the HDR panel, and all of it was gated on the source carrying a PQ or
// HLG transfer characteristic (see VideoDecoder::hdrTransferOf). An SDR
// file therefore had no brightness, contrast, saturation or white-balance
// control at all -- the nits sliders are peak-luminance figures for the
// tone curve, and there is no tone curve on an SDR source to give them a
// meaning. These controls are the part that is meaningful for any source.
//
// Structure mirrors ToneMapper and the audio DSP modules: header-only, no
// state shared with the rest of the pipeline, and an exact no-op unless a
// slider has actually been moved off neutral.
//
// The canonical space is gamma-encoded BT.709 R'G'B' in [0,1], which is
// the one place both video paths can meet:
//
//   * the HDR path is already producing exactly these values, one BT.709
//     OETF lookup before it quantises to 8 bits, so it folds the whole
//     adjustment into that lookup table (see ToneMapper::buildTables);
//   * the SDR path has no such stage, so it converts to RGB24 and runs
//     applyEncoded() over the result.
//
// Same numbers in, same picture out, which is the property that matters:
// a saturation of 1.3 has to look like a saturation of 1.3 whether the
// file it is applied to is HDR10 or an old H.264 rip.
//
// Deliberately NOT done in YUV, which would have been cheaper for the SDR
// path: white balance is a per-channel RGB gain, and in subsampled 4:2:0
// it mixes luma into chroma, so getting it right would have meant
// upsampling chroma to 4:4:4 first. That costs more than the RGB
// conversion it was trying to avoid, and it would have left the two paths
// computing visibly different pictures from the same slider positions.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace naikav {
namespace video {

// ---------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------

struct ColorAdjustSettings {
    // Additive offset on the encoded signal, in 8-bit code units, so the
    // number means the same thing as the code values it shifts. +/-100 is
    // already far past the point of usefulness in either direction.
    float brightness = 0.0f;

    // Multiplier about mid grey (code 128). Below 1 flattens the picture,
    // above 1 stretches it and clips both ends.
    float contrast = 1.0f;

    // Distance of each pixel from its own luma. 0 is monochrome, 1 is
    // untouched, 2 is heavily oversaturated.
    float saturation = 1.0f;

    // White point to render as white, in kelvin. Below 6500 warms the
    // picture, above it cools it -- the same convention as a camera's
    // white-balance dial, and the reason the slider reads backwards to
    // anyone expecting "higher number, warmer".
    float temperatureK = 6500.0f;

    // The axis perpendicular to temperature: negative is green, positive
    // is magenta. Scaled -100..100 to match brightness rather than
    // exposing the raw gain.
    float tint = 0.0f;

    // Whether this is the identity, and so whether the pipeline can skip
    // the extra conversion entirely.
    //
    // Tolerances rather than exact compares, for two reasons: these
    // values round-trip through the settings file as decimal text, and a
    // slider parked a ten-thousandth off neutral must not silently cost a
    // full extra RGB conversion pass on every frame for a change that
    // cannot resolve to even one code value. Each threshold below is
    // roughly "an eighth of a code value at mid grey", which is well
    // under what 8-bit output can represent.
    bool isNeutral() const {
        return std::fabs(brightness) < 0.125f &&
               std::fabs(contrast - 1.0f) < 0.001f &&
               std::fabs(saturation - 1.0f) < 0.001f &&
               std::fabs(temperatureK - 6500.0f) < 1.0f &&
               std::fabs(tint) < 0.05f;
    }

    // Whether any adjustment requires the CPU software pipeline (brightness, contrast, saturation).
    // These non-linear/affine adjustments cannot be applied solely through a single linear
    // texture color multiplier.
    bool requiresSoftwareConversion() const {
        return std::fabs(brightness) >= 0.125f ||
               std::fabs(contrast - 1.0f) >= 0.001f ||
               std::fabs(saturation - 1.0f) >= 0.001f;
    }

    // Whether only white-balance (temperature/tint) is active and can be handled
    // via GPU color modulation with zero CPU software conversion overhead.
    bool isGpuModulationEligible() const {
        return !isNeutral() && !requiresSoftwareConversion();
    }
};

// Slider limits. Kept here rather than in the UI so that the settings
// loader and the UI cannot disagree about what is representable -- a
// hand-edited settings file is otherwise free to put the pipeline
// somewhere no slider can reach or undo.
inline constexpr float kColorBrightnessMin = -100.0f;
inline constexpr float kColorBrightnessMax = 100.0f;
inline constexpr float kColorContrastMin = 0.25f;
inline constexpr float kColorContrastMax = 3.0f;
inline constexpr float kColorSaturationMin = 0.0f;
inline constexpr float kColorSaturationMax = 3.0f;
// The gains below are the real white-point gains, not a cosmetic tint, so
// the ends of this range are genuinely strong: 3000K drives blue down to
// about a quarter. Narrower than the 1667-25000K the approximation below
// is valid over, because past these points the picture is a single hue
// and the control has stopped being useful.
inline constexpr float kColorTemperatureMin = 3000.0f;
inline constexpr float kColorTemperatureMax = 12000.0f;
inline constexpr float kColorTemperatureNeutral = 6500.0f;
inline constexpr float kColorTintMin = -100.0f;
inline constexpr float kColorTintMax = 100.0f;

inline ColorAdjustSettings clampColorAdjust(ColorAdjustSettings s) {
    s.brightness = std::clamp(s.brightness, kColorBrightnessMin, kColorBrightnessMax);
    s.contrast = std::clamp(s.contrast, kColorContrastMin, kColorContrastMax);
    s.saturation = std::clamp(s.saturation, kColorSaturationMin, kColorSaturationMax);
    s.temperatureK =
        std::clamp(s.temperatureK, kColorTemperatureMin, kColorTemperatureMax);
    s.tint = std::clamp(s.tint, kColorTintMin, kColorTintMax);
    return s;
}

// ---------------------------------------------------------------------
// White balance
// ---------------------------------------------------------------------

// Chromaticity of a blackbody radiator at `kelvin`, via the Kim et al.
// cubic approximation of the Planckian locus (valid 1667K-25000K).
//
// Chosen over the piecewise-logarithmic fit that ffmpeg's
// `colortemperature` filter uses because that one is discontinuous at
// 6600K -- its green channel steps by about 2.5% right next to the
// neutral point, which on a slider that has to pass through 6500K on its
// way between warm and cool is a visible jump in the picture. Both
// branches of this fit agree to within 1e-4 where they meet at 4000K.
inline void planckianXy(double kelvin, double& x, double& y) {
    kelvin = std::clamp(kelvin, 1667.0, 25000.0);
    const double t = kelvin;
    const double t2 = t * t;
    const double t3 = t2 * t;

    if (t <= 4000.0) {
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
    } else {
        x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    }

    const double x2 = x * x;
    const double x3 = x2 * x;
    if (t <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
}

// Linear-light RGB of a chromaticity, normalised to Y = 1.
inline void xyToLinearRgb(double x, double y, double& r, double& g, double& b) {
    // A degenerate y would divide by zero; the locus never goes near it,
    // but the clamp costs nothing and keeps a hand-edited settings file
    // from producing infinities.
    y = std::max(y, 1e-6);
    const double X = x / y;
    const double Y = 1.0;
    const double Z = (1.0 - x - y) / y;

    // XYZ -> linear sRGB/BT.709 (both share the same primaries).
    r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
}

// Per-channel gains for a temperature/tint pair.
//
// Two normalisations, both load-bearing:
//
//   1. Against the locus value at 6500K, so that the neutral slider
//      position is the exact identity. Taking the raw gains would leave a
//      small permanent cast at "neutral", because the Planckian point at
//      6500K is not quite D65 and the approximation is not exact either.
//   2. Against the resulting luma, so that moving the slider changes the
//      colour of the picture and not its brightness -- otherwise the
//      temperature control doubles as a second, worse brightness control
//      and the two fight each other.
inline void whiteBalanceGains(float kelvin, float tint,
                              float& gr, float& gg, float& gb) {
    double x = 0.0, y = 0.0;
    planckianXy(kelvin, x, y);
    double r = 0.0, g = 0.0, b = 0.0;
    xyToLinearRgb(x, y, r, g, b);

    double nx = 0.0, ny = 0.0;
    planckianXy(kColorTemperatureNeutral, nx, ny);
    double nr = 0.0, ng = 0.0, nb = 0.0;
    xyToLinearRgb(nx, ny, nr, ng, nb);

    // The locus stays well inside the positive octant over the range
    // above, so these floors never bind in practice; they exist so that a
    // settings file claiming some absurd temperature cannot produce a
    // negative or infinite gain.
    constexpr double kFloor = 1e-4;
    r /= std::max(nr, kFloor);
    g /= std::max(ng, kFloor);
    b /= std::max(nb, kFloor);

    // Tint: push green one way and the red/blue pair the other, which is
    // the green-magenta axis. 0.25 at full deflection lands in the same
    // visual ballpark as the ends of the temperature range, so neither
    // slider feels dead next to the other.
    const double t = std::clamp(static_cast<double>(tint) / 100.0, -1.0, 1.0);
    r *= 1.0 + 0.25 * t;
    g *= 1.0 - 0.25 * t;
    b *= 1.0 + 0.25 * t;

    const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double norm = (luma > kFloor) ? (1.0 / luma) : 1.0;

    gr = static_cast<float>(r * norm);
    gg = static_cast<float>(g * norm);
    gb = static_cast<float>(b * norm);
}

// Returns normalized GPU texture color modulation factors (0.0 .. 1.0) for
// white balance (temperature and tint). When only white-balance is active,
// applying these factors via SDL_SetTextureColorModFloat() executes entirely on
// the GPU with zero CPU overhead, keeping the frame on the native zero-copy path.
inline void getGpuColorModulation(const ColorAdjustSettings& s,
                                  float& rMod, float& gMod, float& bMod) {
    if (s.isNeutral() || s.requiresSoftwareConversion()) {
        rMod = gMod = bMod = 1.0f;
        return;
    }
    float gr = 1.0f, gg = 1.0f, gb = 1.0f;
    whiteBalanceGains(s.temperatureK, s.tint, gr, gg, gb);
    const float maxG = std::max({gr, gg, gb, 1e-4f});
    rMod = std::clamp(gr / maxG, 0.0f, 1.0f);
    gMod = std::clamp(gg / maxG, 0.0f, 1.0f);
    bMod = std::clamp(gb / maxG, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------
// The transform, in the form both consumers actually want
// ---------------------------------------------------------------------

// The whole chain as it acts on one gamma-encoded channel value e in
// [0,1]:
//
//   e -> gain[c] * e                    white balance (temperature, tint)
//     -> contrast * (v - 0.5) + 0.5     contrast, about mid grey
//     -> v + brightness/255             brightness
//     -> luma + (v - luma) * saturation saturation
//
// The first three are per-channel affine and collapse into a single
// multiply-add per channel:
//
//   v = scale[c] * e + pedestal
//   scale[c] = contrast * gain[c]
//   pedestal = 0.5 - 0.5 * contrast + brightness / 255
//
// which matters because it means any consumer that already has a
// per-channel lookup table producing e can fold the entire white
// balance, contrast and brightness stage into that table and pay nothing
// per pixel. Saturation is the only stage that reads more than one
// channel, so it is the only one that has to stay a real per-pixel step
// -- and `mixesChannels` says when even that can be skipped.
//
// Saturation is applied last, and after the pedestal, on purpose: the
// pedestal is the same on all three channels, so it shifts a pixel along
// the neutral axis without changing its distance from that axis. That
// makes the two stages commute for the offset and lets the fold above be
// exact rather than an approximation.
struct EncodedColorTransform {
    bool active = false;         // false => exactly the identity
    bool mixesChannels = false;  // false => saturation is 1, skip that step
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float pedestal = 0.0f;
    float saturation = 1.0f;

    // BT.709 luma weights -- the output of both paths is BT.709, so this
    // is the luma the saturation step must preserve.
    static constexpr float kLumaR = 0.2126f;
    static constexpr float kLumaG = 0.7152f;
    static constexpr float kLumaB = 0.0722f;

    // Apply to one pixel of gamma-encoded RGB in [0,1]. Deliberately not
    // clamped: the HDR path folds `scale`/`pedestal` into a table and
    // calls only the saturation half, and clamping in the middle of that
    // would crush values that the saturation step was about to bring back
    // into range.
    void applyEncoded(float& r, float& g, float& b) const {
        r = scale[0] * r + pedestal;
        g = scale[1] * g + pedestal;
        b = scale[2] * b + pedestal;
        if (mixesChannels) {
            applySaturation(r, g, b);
        }
    }

    void applySaturation(float& r, float& g, float& b) const {
        const float luma = kLumaR * r + kLumaG * g + kLumaB * b;
        r = luma + (r - luma) * saturation;
        g = luma + (g - luma) * saturation;
        b = luma + (b - luma) * saturation;
    }
};

// Bitwise equality despite being a float compare: the question is not
// "are these two values close" but "has the UI written a new value since
// the last frame", and the settings arrive as a copy of the same atomics
// every time. Both the SDR adjuster and the tone mapper guard their table
// rebuilds on this.
inline bool sameColorAdjust(const ColorAdjustSettings& a,
                            const ColorAdjustSettings& b) {
    return a.brightness == b.brightness && a.contrast == b.contrast &&
           a.saturation == b.saturation && a.temperatureK == b.temperatureK &&
           a.tint == b.tint;
}

inline EncodedColorTransform
makeEncodedColorTransform(const ColorAdjustSettings& raw) {
    EncodedColorTransform t;
    if (raw.isNeutral()) {
        return t;
    }

    const ColorAdjustSettings s = clampColorAdjust(raw);

    float gr = 1.0f, gg = 1.0f, gb = 1.0f;
    whiteBalanceGains(s.temperatureK, s.tint, gr, gg, gb);

    t.active = true;
    t.scale[0] = s.contrast * gr;
    t.scale[1] = s.contrast * gg;
    t.scale[2] = s.contrast * gb;
    t.pedestal = 0.5f - 0.5f * s.contrast + s.brightness / 255.0f;
    t.saturation = s.saturation;
    // Same tolerance as isNeutral() uses, so that a saturation slider
    // sitting on neutral costs nothing even when the other sliders have
    // put the transform into play.
    t.mixesChannels = std::fabs(s.saturation - 1.0f) >= 0.001f;
    return t;
}

// ---------------------------------------------------------------------
// ColorAdjuster
// ---------------------------------------------------------------------

// Applies the transform to a packed RGB24 image in place. This is the SDR
// path's half of the feature; the HDR path folds the same transform into
// the tone mapper's output table instead (ToneMapper::configure).
class ColorAdjuster {
public:
    ColorAdjuster() = default;

    // Returns whether the adjuster now has anything to do. Cheap enough
    // to call per frame -- it is a handful of transcendentals at worst,
    // and it short-circuits when the settings have not moved.
    bool configure(const ColorAdjustSettings& settings) {
        if (m_configured && sameColorAdjust(settings, m_settings)) {
            return m_transform.active;
        }
        m_settings = settings;
        m_transform = makeEncodedColorTransform(settings);
        if (m_transform.active) {
            buildTables();
        }
        m_configured = true;
        return m_transform.active;
    }

    bool isActive() const { return m_transform.active; }
    const EncodedColorTransform& transform() const { return m_transform; }
    const ColorAdjustSettings& settings() const { return m_settings; }

    // Packed RGB24, `stride` bytes per row, adjusted in place.
    //
    // Split across threads by row on the same reasoning as
    // ToneMapper::process: the rows are independent, and at 4K this is
    // several million pixels of straight-line float arithmetic.
    void processRgb24(uint8_t* data, int stride, int width, int height,
                      int maxWorkers = 0) const {
        if (!m_transform.active || !data || width <= 0 || height <= 0) {
            return;
        }

        int workers = chooseWorkerCount(width, height, m_transform.mixesChannels);
        if (maxWorkers > 0) {
            workers = std::max(1, std::min(workers, maxWorkers));
        }
        if (workers <= 1) {
            processRows(data, stride, width, 0, height);
            return;
        }

        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(workers) - 1);
        const int rowsPer = (height + workers - 1) / workers;
        for (int w = 1; w < workers; ++w) {
            const int y0 = w * rowsPer;
            if (y0 >= height) break;
            const int y1 = std::min(height, y0 + rowsPer);
            pool.emplace_back([this, data, stride, width, y0, y1]() {
                processRows(data, stride, width, y0, y1);
            });
        }
        processRows(data, stride, width, 0, std::min(height, rowsPer));
        for (auto& th : pool) {
            th.join();
        }
    }

private:
    ColorAdjustSettings m_settings{};
    EncodedColorTransform m_transform{};
    bool m_configured = false;

    // The per-channel half of the transform, precomputed over all 256
    // input codes. The input here is an 8-bit code, not a continuous
    // value, so the "table" is the entire domain -- there is no
    // approximation in this, it is the same arithmetic done 256 times
    // instead of once per pixel.
    //
    // Two forms, because the two cases want different things:
    //   m_lut8  -- when saturation is neutral the whole transform is
    //              per-channel, so a pixel is three byte lookups and no
    //              float math at all.
    //   m_lutF  -- when it is not, the three channels still have to meet
    //              for the luma, so they are kept in float and left
    //              unclamped until after that step.
    std::array<std::array<uint8_t, 256>, 3> m_lut8{};
    std::array<std::array<float, 256>, 3> m_lutF{};

    void buildTables() {
        for (int c = 0; c < 3; ++c) {
            for (int v = 0; v < 256; ++v) {
                const float e = static_cast<float>(v) * (1.0f / 255.0f);
                const float out = m_transform.scale[c] * e + m_transform.pedestal;
                m_lutF[c][v] = out;
                m_lut8[c][v] = quantize(out);
            }
        }
    }

    // Same shape of trade as ToneMapper::chooseWorkerCount, but the
    // numbers land somewhere else and the fixed cost matters more here.
    //
    // This loop is a few float ops per pixel -- perhaps a tenth of what
    // the tone mapper does -- while spawning and joining a thread costs
    // the same tens of microseconds either way. Taking the tone mapper's
    // "all cores past 65k pixels" rule literally meant eight threads for
    // a 640x360 frame, where the spawn cost more than the work it was
    // splitting. So the count scales with the work instead of jumping
    // straight to the full complement: a worker has to have about half a
    // megapixel of its own before it is worth having. That leaves
    // anything up to roughly 720p single-threaded and still gives a 4K
    // frame every core.
    // Two rates, because the two row loops differ by about an order of
    // magnitude per pixel. The byte-lookup loop is three table reads and
    // is essentially memory-bound, so extra workers buy little and the
    // spawn dominates until the frame is large; the saturation loop is
    // real arithmetic and wants every core it can get, on the same
    // reasoning (and roughly the same threshold) as
    // ToneMapper::chooseWorkerCount.
    static constexpr long long kPixelsPerWorkerLut = 1 << 19;
    static constexpr long long kPixelsPerWorkerMix = 1 << 16;

    static int chooseWorkerCount(int width, int height, bool mixesChannels) {
        const long long pixels = static_cast<long long>(width) * height;
        const long long per =
            mixesChannels ? kPixelsPerWorkerMix : kPixelsPerWorkerLut;
        if (pixels < per) return 1;
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        int workers = static_cast<int>(std::min<unsigned>(hw, 8u));
        workers = std::min(workers, static_cast<int>(pixels / per));
        return std::max(1, std::min(workers, height));
    }

    // Two loops rather than one with a test inside it. The test is
    // uniform over the whole frame, but left in the body it is a branch
    // per pixel that stops the compiler vectorising anything around it --
    // measured at roughly twice the cost of the arithmetic it was
    // guarding.
    void processRows(uint8_t* data, int stride, int width, int y0,
                     int y1) const {
        if (!m_transform.mixesChannels) {
            for (int y = y0; y < y1; ++y) {
                uint8_t* row = data + static_cast<size_t>(y) * stride;
                for (int x = 0; x < width; ++x) {
                    uint8_t* p = row + static_cast<size_t>(x) * 3;
                    p[0] = m_lut8[0][p[0]];
                    p[1] = m_lut8[1][p[1]];
                    p[2] = m_lut8[2][p[2]];
                }
            }
            return;
        }

        const float sat = m_transform.saturation;
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = data + static_cast<size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                uint8_t* p = row + static_cast<size_t>(x) * 3;
                const float r = m_lutF[0][p[0]];
                const float g = m_lutF[1][p[1]];
                const float b = m_lutF[2][p[2]];
                const float luma = EncodedColorTransform::kLumaR * r +
                                   EncodedColorTransform::kLumaG * g +
                                   EncodedColorTransform::kLumaB * b;
                p[0] = quantize(luma + (r - luma) * sat);
                p[1] = quantize(luma + (g - luma) * sat);
                p[2] = quantize(luma + (b - luma) * sat);
            }
        }
    }

    // min/max rather than std::clamp: the same result, but it compiles to
    // two SSE instructions instead of the comparison chain std::clamp's
    // reference-returning form produces, and this sits in the inner loop.
    static uint8_t quantize(float v) {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint8_t>(static_cast<int>(c * 255.0f + 0.5f));
    }
};

}  // namespace video
}  // namespace naikav
