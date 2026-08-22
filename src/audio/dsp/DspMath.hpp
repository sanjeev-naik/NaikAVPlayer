#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define NAIKAV_DSP_HAS_MXCSR 1
  #include <xmmintrin.h>
  #include <pmmintrin.h>
#else
  #define NAIKAV_DSP_HAS_MXCSR 0
#endif

namespace naikav::dsp {

// Pi, defined locally rather than taken from <cmath>'s M_PI.
//
// M_PI is a POSIX extension, not standard C++. libstdc++ exposes it from
// <cmath> by default, but MSVC only defines it when _USE_MATH_DEFINES is
// set *before* the first <cmath>/<math.h> include -- a whole-translation-
// unit ordering constraint that a header-only module in a shared include
// path cannot reliably guarantee. (SDL3's SDL_stdinc.h also defines M_PI
// as a fallback, which is why the test binary, which includes SDL before
// these headers, used to build on MSVC while every other target failed.)
//
// A constexpr constant sidesteps the ordering problem entirely and is
// exactly as precise: long double's mantissa cannot represent more digits
// than are written here.
inline constexpr double kPi = 3.14159265358979323846;

// Scoped flush-to-zero / denormals-are-zero for the calling thread.
//
// Every IIR stage in this folder (Biquad, and therefore ParametricEQ,
// Crossover, MultibandCompressor, SpatialDownmixer, Surround3D) has state
// that decays exponentially toward zero when the input goes silent. On x86
// those values enter the denormal range and stay there, and denormal
// arithmetic traps to microcode: measured at 18x the cost of the same
// chain processing normal programme material, and 54x for ParametricEQ
// alone. That spike lands exactly on fade-outs, silent leaders and gaps
// between tracks -- the passages where a dropout is most audible.
//
// Denormal handling is a per-thread CPU mode rather than a property of the
// code, and the audio callback thread is created by SDL, so nothing would
// otherwise ever set it. Construct one of these at the top of the audio
// callback; the destructor restores the caller's mode so this never leaks
// into SDL's or the UI's floating-point behavior.
//
// A no-op on non-x86 targets, where MXCSR does not exist -- Biquad carries
// its own explicit state flush for those (see flushDenormal below).
class ScopedDenormalGuard {
public:
    ScopedDenormalGuard() {
#if NAIKAV_DSP_HAS_MXCSR
        m_saved = _mm_getcsr();
        // 0x8000 = flush-to-zero (denormal results become zero)
        // 0x0040 = denormals-are-zero (denormal inputs are treated as zero)
        _mm_setcsr((m_saved | 0x8000u | 0x0040u));
#endif
    }
    ~ScopedDenormalGuard() {
#if NAIKAV_DSP_HAS_MXCSR
        _mm_setcsr(m_saved);
#endif
    }

    ScopedDenormalGuard(const ScopedDenormalGuard&) = delete;
    ScopedDenormalGuard& operator=(const ScopedDenormalGuard&) = delete;

private:
#if NAIKAV_DSP_HAS_MXCSR
    unsigned int m_saved = 0;
#endif
};

// Portable backstop for the guard above: forces a filter state value to
// exactly zero once it decays below the smallest normal float. On x86 the
// MXCSR guard has already done this, so this costs one predictable branch;
// on targets without MXCSR it is what keeps the IIR tails cheap.
inline float flushDenormal(float v) {
    return (std::fabs(v) < 1e-30f) ? 0.0f : v;
}

// ---------------------------------------------------------------------
// Fast dB <-> linear conversion.
//
// The dynamics stages (Compressor, NoiseGate, Limiter, and three more
// inside MultibandCompressor) each convert to dB with std::log10 and back
// with std::pow once per frame. With the full chain enabled that is 12
// transcendental calls per frame, ~576k per second at 48kHz, on the audio
// callback thread.
//
// These replacements work on the float's exponent/mantissa fields
// directly: extract the exponent for the integer part of log2, then a
// minimax polynomial over the [1,2) mantissa for the fraction. Accuracy is
// well under 0.01 dB across the useful range -- far below any audible
// threshold for a gain computer, and below the resolution of the dB
// parameters the user can actually set -- for roughly 5-10x the throughput
// of the libm calls.
// ---------------------------------------------------------------------

inline float fastLog2(float x) {
    // Guard the domain. Callers add a small epsilon before calling, but a
    // denormal-flushed zero can still arrive here, and the exponent
    // extraction below is only valid for normal floats -- a subnormal has
    // a zero exponent field and an unnormalized mantissa, so it would
    // decode to nonsense rather than merely losing precision.
    if (!(x > 1.17549435e-38f)) return -126.0f;

    uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127;

    // Replace the exponent with 0 so the value lands in [1, 2).
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &bits, sizeof(m));

    // log2(m) via the inverse-hyperbolic-tangent series. With
    // t = (m-1)/(m+1), m in [1,2) maps to t in [0, 1/3], where
    //   ln(m) = 2 * (t + t^3/3 + t^5/5 + t^7/7 + ...)
    // Because t stays small the series converges quickly: truncating
    // after t^7 leaves well under 0.001 dB across the whole interval,
    // and unlike a plain polynomial fit in m it is accurate in the
    // interior rather than only near the endpoints.
    const float t = (m - 1.0f) / (m + 1.0f);
    const float t2 = t * t;
    float s = 0.142857143f;   // 1/7
    s = s * t2 + 0.2f;        // 1/5
    s = s * t2 + 0.333333333f;// 1/3
    s = s * t2 + 1.0f;

    return static_cast<float>(exponent) + t * s * 2.885390082f; // 2/ln(2)
}

inline float fastExp2(float x) {
    // Clamp to the range a float exponent can represent, so an extreme
    // gain-reduction value can never produce inf/nan.
    x = std::clamp(x, -126.0f, 126.0f);

    const float floorX = std::floor(x);
    const int i = static_cast<int>(floorX);
    const float f = x - floorX;               // fractional part, [0, 1)

    // Degree-4 minimax fit of 2^f over f in [0,1).
    float p = 0.0136699758f;
    p = p * f + 0.0517692170f;
    p = p * f + 0.2415034135f;
    p = p * f + 0.6931471805f;
    p = p * f + 1.0f;

    // Scale by 2^i via direct exponent construction.
    uint32_t bits = static_cast<uint32_t>((i + 127)) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof(scale));
    return p * scale;
}

// 20*log10(x) == 20/log2(10) * log2(x)
inline float fastLinearToDb(float linear) {
    return 6.0205999132f * fastLog2(linear);
}

// 10^(db/20) == 2^(db / (20*log10(2)))
inline float fastDbToLinear(float db) {
    return fastExp2(db * 0.1660964048f);
}

// ---------------------------------------------------------------------
// Shared level detector for the dynamics stages.
//
// A raw per-frame peak detector sees a sustained tone cross zero twice per
// cycle and reads that as "below threshold" every cycle, which makes a
// gate chatter and makes a compressor's gain envelope ripple at twice the
// signal frequency -- amplitude modulation, i.e. added harmonic and
// intermodulation distortion, worst on low-frequency content where the
// period is long relative to the attack time.
//
// NoiseGate previously carried a symmetric one-pole to fix this for
// itself; Compressor had none at all. This is that detector, factored out
// and made asymmetric: attack is instantaneous so a genuine transient is
// tracked with no delay, release is smoothed over kReleaseMs so the
// detector rides through a cycle's zero-crossing dip. The symmetric
// version delayed the gate's deliberately-fast open by the full 8ms,
// blunting exactly the onsets that fast attack was chosen to protect.
//
// This is a PEAK detector, so every threshold driven by it is
// peak-referenced -- worth stating plainly, because most compressor UIs
// imply programme/RMS level and the two differ by the signal's crest
// factor. Measured against a -20 dBFS threshold at 4:1, gain reduction
// reaches 1 dB at -21.75 dBFS RMS on a sine (crest 3.0 dB) and at
// -18.75 dBFS RMS on a square (crest 0 dB): within a couple of dB for
// continuous material, because the instantaneous attack tracks the peak
// and the peak is what is compared. Transient-heavy material with a high
// crest factor engages at a correspondingly lower *programme* level --
// which is the intended behaviour for catching transients, but is not
// what a user reading the dial as "average level" would predict.
//
// Deliberately not changed to RMS: peak detection is what lets the fast
// attack catch a transient before it lands, and re-referencing the
// threshold would silently retune every existing preset and saved
// setting. The mismatch is a labelling problem, so it is fixed with
// labelling -- see the Compressor/NoiseGate threshold tooltips.
class LevelDetector {
public:
    void configure(double sampleRate) {
        m_coeff = (sampleRate > 0.0)
            ? std::exp(-1.0f / (static_cast<float>(sampleRate) * (kReleaseMs / 1000.0f)))
            : 0.0f;
    }

    void reset() { m_level = 0.0f; }

    // Instantaneous attack, smoothed release.
    inline float process(float peak) {
        m_level = (peak > m_level)
            ? peak
            : m_coeff * m_level + (1.0f - m_coeff) * peak;
        m_level = flushDenormal(m_level);
        return m_level;
    }

    float level() const { return m_level; }

private:
    static constexpr float kReleaseMs = 8.0f;
    float m_coeff = 0.0f;
    float m_level = 0.0f;
};

// Peak magnitude across one interleaved frame.
inline float framePeak(const float* frame, int channels) {
    float peak = 0.0f;
    for (int ch = 0; ch < channels; ++ch) {
        peak = std::max(peak, std::fabs(frame[ch]));
    }
    return peak;
}

// Rounds up to the next power of two, so ring buffers can wrap with a
// mask instead of an integer division. `%` against a runtime-variable
// size compiles to a div -- tens of cycles, once or more per sample per
// channel in every delay line in this folder.
inline int nextPowerOfTwo(int n) {
    if (n < 1) return 1;
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// ---------------------------------------------------------------------
// Bypass crossfade for a DSP stage's enable/disable transition.
//
// Every stage here used to switch on a bare bool, taking effect on the
// very next sample. That is a step discontinuity in the waveform -- a
// click -- and for the IIR stages it is worse than that: their filter
// state is whatever it was when they were last switched off, so
// re-enabling resumes from arbitrarily old history. Biquad already ramps
// *coefficient* changes for exactly this reason; the enable flags went
// straight around that machinery.
//
// This fades between the stage's processed output and the dry input it
// was handed, over kFadeMs. Two details matter:
//
//   - The fade is linear, not equal-power. Equal-power is right when
//     crossfading two *uncorrelated* sources; here the wet signal is a
//     processed copy of the dry one, so the two are strongly correlated
//     and a linear blend is what actually holds the level constant. An
//     equal-power law on correlated signals peaks at sqrt(a)+sqrt(1-a),
//     i.e. +3.01 dB halfway through -- an audible level bump in the
//     middle of a transition whose whole purpose is to be inaudible.
//
//   - A stage that has never processed audio snaps instead of fading, so
//     configuring a stage before playback starts gives its full effect
//     from the first sample. Same idiom, and same reasoning, as
//     Biquad::markPrimed().
class BypassCrossfade {
public:
    void configure(double sampleRate) {
        m_fadeSamples = (sampleRate > 0.0)
            ? std::max(1, static_cast<int>(std::lround(sampleRate * (kFadeMs / 1000.0))))
            : 1;
    }

    // Snap to the current target and forget that audio ever ran, so the
    // next enable change applies instantly rather than gliding in from
    // pre-reset state.
    void reset() {
        m_mix = m_target;
        m_primed = false;
    }

    void setEnabled(bool on) {
        m_target = on ? 1.0f : 0.0f;
        if (!m_primed) m_mix = m_target;
    }
    bool isEnabled() const { return m_target >= 0.5f; }

    // True when the stage can be skipped outright: switched off, and the
    // fade-out has already finished.
    bool isInactive() const { return m_target == 0.0f && m_mix == 0.0f; }
    bool isFading() const { return m_mix != m_target; }
    void markPrimed() { m_primed = true; }
    void snap() { m_mix = m_target; }

    // Blends `processed` (in place) back toward `dry` according to where
    // the fade has got to, advancing one step per frame.
    void blend(float* processed, const float* dry, int numFrames, int channels) {
        if (channels <= 0) return;
        const float step = 1.0f / static_cast<float>(m_fadeSamples);
        for (int f = 0; f < numFrames; ++f) {
            if (m_mix < m_target) {
                m_mix = std::min(m_target, m_mix + step);
            } else if (m_mix > m_target) {
                m_mix = std::max(m_target, m_mix - step);
            }
            const float wetGain = m_mix;
            const float dryGain = 1.0f - m_mix;
            float* out = processed + static_cast<size_t>(f) * channels;
            const float* in = dry + static_cast<size_t>(f) * channels;
            for (int c = 0; c < channels; ++c) {
                out[c] = wetGain * out[c] + dryGain * in[c];
            }
        }
    }

private:
    static constexpr float kFadeMs = 8.0f; // inside the 5-10 ms the ear reads as instant
    int m_fadeSamples = 1;
    float m_mix = 0.0f;    // 0 = fully dry (bypassed), 1 = fully wet
    float m_target = 0.0f;
    bool m_primed = false;
};

// Scratch buffer a stage keeps for its bypass crossfade, sized once by
// reserveBlock() so process() never reaches the allocator. When it is too
// small (only possible if reserveBlock was never called for the block size
// in use) the owning stage snaps the fade instead of allocating -- a click
// is bad, an allocation on the audio thread is worse.
class BypassScratch {
public:
    void reserve(int maxFrames, int channels) {
        if (maxFrames <= 0 || channels <= 0) return;
        const size_t total = static_cast<size_t>(maxFrames) * static_cast<size_t>(channels);
        if (m_buf.size() < total) m_buf.resize(total);
    }
    bool fits(size_t total) const { return m_buf.size() >= total; }
    float* data() { return m_buf.data(); }
    const float* data() const { return m_buf.data(); }

private:
    std::vector<float> m_buf;
};

// True when every value is finite. Used to reject a parameter that would
// poison an IIR filter's feedback state permanently -- NaN propagates
// through y[n-1]/y[n-2] forever, so a filter that once sees one never
// recovers without an explicit reset.
inline bool isFinitePositive(double v) {
    return std::isfinite(v) && v > 0.0;
}

} // namespace naikav::dsp
