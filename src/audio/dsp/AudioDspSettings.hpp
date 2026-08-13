#pragma once

#include "audio/dsp/ParametricEQ.hpp"
#include <string>
#include <algorithm>
#include <cctype>

namespace naikav::dsp {

// Plain, serializable snapshot of every user-facing audio DSP/loudness
// parameter. Exists so PlayerController/PlayerUI/settings-file persistence
// can pass the whole configuration around and apply it atomically in one
// call (AudioDecoder::applyDspSettings()), rather than needing a dozen
// separate setter round-trips -- and so a "preset" is just a canned value
// of this struct.
//
// compressorEnabled/limiterEnabled aren't separate flags on Compressor/
// Limiter themselves (those classes stay minimal); "disabled" is encoded
// as the parameter value that makes each stage a no-op (compressor ratio
// 1:1, limiter ceiling 0dB) -- see AudioDecoder::applyDspSettings().
struct AudioDspSettings {
    bool dspEnabled = false;

    float eqBandGainDb[ParametricEQ::kNumBands] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    // Per-band center frequency/Q, matching ParametricEQ's own defaults --
    // present here (rather than only living inside ParametricEQ) because
    // the EQ is now truly parametric: users can move a band's frequency
    // and width, not just its gain, and that has to round-trip through
    // settings persistence/presets like every other DSP parameter.
    float eqBandFreqHz[ParametricEQ::kNumBands] = {60.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};
    float eqBandQ[ParametricEQ::kNumBands] = {0.9f, 1.0f, 1.0f, 1.0f, 0.9f};

    bool compressorEnabled = false;
    float compressorThresholdDb = -20.0f;
    float compressorRatio = 4.0f;

    bool limiterEnabled = false;
    float limiterCeilingDb = -1.0f;

    bool crossoverEnabled = false;
    float crossoverCutoffHz = 120.0f;
    // True bass management: redirect content below crossoverCutoffHz from
    // every non-LFE channel into the LFE channel instead of just taming
    // the LFE channel in isolation. See Crossover::setBassRedirectEnabled().
    bool crossoverBassRedirectEnabled = false;

    bool loudnessEnabled = false;
    float loudnessTargetLufs = -16.0f; // streaming-style default

    // Mid-side stereo widener -- see StereoWidener.hpp. Only audible on
    // 2-channel output; a no-op on native multichannel passthrough.
    bool widenerEnabled = false;
    float widenerWidth = 1.5f; // 1.0 = unity; default is a noticeable-but-tasteful widen once enabled

    // "3D Surround" ambience synthesis -- see Surround3D.hpp. Works on any
    // 2-channel output, including plain stereo sources (unlike
    // AudioChannelOption::VIRTUAL_SURROUND, which needs a real discrete
    // multichannel source to fold down).
    bool surround3dEnabled = false;
    float surround3dIntensity = 1.0f; // 1.0 = designed nominal strength

    // Left/right output balance -- see BalanceControl.hpp. Only audible on
    // 2-channel output; a no-op on native multichannel passthrough.
    float balance = 0.0f; // 0 = centered/inert by default

    // Downward noise gate/expander -- see NoiseGate.hpp. "Disabled" is
    // encoded as ratio 1:1 (a true no-op), matching compressorEnabled's
    // convention above rather than a separate flag on NoiseGate itself.
    bool noiseGateEnabled = false;
    float noiseGateThresholdDb = -50.0f;
    float noiseGateRatio = 4.0f;

    // Multiband compression (low/mid/high) -- see MultibandCompressor.hpp.
    // Each band's "disabled" state is likewise ratio 1:1; multibandEnabled
    // gates the whole feature (matching DspChain's own master switch).
    bool multibandEnabled = false;
    float multibandLowMidHz = 250.0f;
    float multibandMidHighHz = 4000.0f;
    float multibandLowThresholdDb = -20.0f;
    float multibandLowRatio = 1.0f;
    float multibandMidThresholdDb = -20.0f;
    float multibandMidRatio = 1.0f;
    float multibandHighThresholdDb = -20.0f;
    float multibandHighRatio = 1.0f;

    // When enabled, PlayerController automatically swaps in a preset that
    // matches the current file's genre tag (if any) on file open -- see
    // PlayerController::applyGenrePresetIfEnabled(). Off by default since
    // it overwrites whatever DSP settings the user currently has dialed
    // in; opt-in convenience, not a silent default behavior change.
    bool autoGenrePresetEnabled = false;

    // Real-time magnitude spectrum visualizer -- see SpectrumAnalyzer.hpp.
    // A display feature, not an audio effect (it never modifies the
    // signal), but still gated behind its own enable flag like everything
    // else here so it costs nothing until turned on.
    bool spectrumAnalyzerEnabled = false;

    bool operator==(const AudioDspSettings& other) const {
        if (dspEnabled != other.dspEnabled ||
            compressorEnabled != other.compressorEnabled ||
            compressorThresholdDb != other.compressorThresholdDb ||
            compressorRatio != other.compressorRatio ||
            limiterEnabled != other.limiterEnabled ||
            limiterCeilingDb != other.limiterCeilingDb ||
            crossoverEnabled != other.crossoverEnabled ||
            crossoverCutoffHz != other.crossoverCutoffHz ||
            crossoverBassRedirectEnabled != other.crossoverBassRedirectEnabled ||
            loudnessEnabled != other.loudnessEnabled ||
            loudnessTargetLufs != other.loudnessTargetLufs ||
            widenerEnabled != other.widenerEnabled ||
            widenerWidth != other.widenerWidth ||
            surround3dEnabled != other.surround3dEnabled ||
            surround3dIntensity != other.surround3dIntensity ||
            balance != other.balance ||
            noiseGateEnabled != other.noiseGateEnabled ||
            noiseGateThresholdDb != other.noiseGateThresholdDb ||
            noiseGateRatio != other.noiseGateRatio ||
            multibandEnabled != other.multibandEnabled ||
            multibandLowMidHz != other.multibandLowMidHz ||
            multibandMidHighHz != other.multibandMidHighHz ||
            multibandLowThresholdDb != other.multibandLowThresholdDb ||
            multibandLowRatio != other.multibandLowRatio ||
            multibandMidThresholdDb != other.multibandMidThresholdDb ||
            multibandMidRatio != other.multibandMidRatio ||
            multibandHighThresholdDb != other.multibandHighThresholdDb ||
            multibandHighRatio != other.multibandHighRatio ||
            autoGenrePresetEnabled != other.autoGenrePresetEnabled ||
            spectrumAnalyzerEnabled != other.spectrumAnalyzerEnabled) {
            return false;
        }
        for (int i = 0; i < ParametricEQ::kNumBands; ++i) {
            if (eqBandGainDb[i] != other.eqBandGainDb[i]) return false;
            if (eqBandFreqHz[i] != other.eqBandFreqHz[i]) return false;
            if (eqBandQ[i] != other.eqBandQ[i]) return false;
        }
        return true;
    }
    bool operator!=(const AudioDspSettings& other) const { return !(*this == other); }
};

// Canned presets covering common listening scenarios. Each one sets
// *every* field explicitly (rather than leaving some at whatever the
// struct's default happens to be) since PlayerUI replaces the whole
// settings struct wholesale when a preset button is pressed -- an
// omitted field here would silently inherit AudioDspSettings{}'s default
// instead of a value this preset actually intends.
inline AudioDspSettings makeFlatPreset() {
    return AudioDspSettings{}; // all defaults: DSP off, nothing colored
}

inline AudioDspSettings makeMusicPreset() {
    AudioDspSettings s;
    s.dspEnabled = true;
    s.eqBandGainDb[0] = 2.0f;  // gentle bass lift
    s.eqBandGainDb[4] = 1.5f;  // gentle treble lift ("smile" EQ)
    s.compressorEnabled = true;
    s.compressorThresholdDb = -18.0f;
    s.compressorRatio = 2.0f; // light touch, preserve dynamics
    s.limiterEnabled = true;
    s.limiterCeilingDb = -0.5f;
    s.loudnessEnabled = true;
    s.loudnessTargetLufs = -14.0f; // typical streaming-music target
    s.widenerEnabled = true;
    s.widenerWidth = 1.3f; // moderate -- widen the stereo image without smearing the mix
    s.surround3dEnabled = false; // most mixes are already well-balanced; leave the artist's image alone
    s.surround3dIntensity = 1.0f;
    return s;
}

inline AudioDspSettings makeCinemaPreset() {
    AudioDspSettings s;
    s.dspEnabled = true;
    s.eqBandGainDb[2] = 3.0f; // dialogue presence (~1kHz band)
    s.compressorEnabled = true;
    s.compressorThresholdDb = -24.0f;
    s.compressorRatio = 3.0f; // tame explosion/music-bed swings under dialogue
    s.limiterEnabled = true;
    s.limiterCeilingDb = -1.0f;
    s.crossoverEnabled = true;
    s.crossoverCutoffHz = 100.0f;
    s.loudnessEnabled = true;
    s.loudnessTargetLufs = -23.0f; // EBU R128 broadcast/film target
    s.widenerEnabled = true;
    s.widenerWidth = 1.2f;
    s.surround3dEnabled = true; // biggest win here: pairs with a real 5.1/7.1 source folded down
    s.surround3dIntensity = 1.0f; // by AudioChannelOption::VIRTUAL_SURROUND, or upmixes a stereo mix
    return s;
}

inline AudioDspSettings makeNightPreset() {
    AudioDspSettings s;
    s.dspEnabled = true;
    s.eqBandGainDb[2] = 2.0f; // keep dialogue legible at low volume
    s.compressorEnabled = true;
    s.compressorThresholdDb = -30.0f; // catch quiet passages, not just peaks
    s.compressorRatio = 6.0f;         // heavy: this is the point of "night mode"
    s.limiterEnabled = true;
    s.limiterCeilingDb = -1.0f;
    s.loudnessEnabled = true;
    s.loudnessTargetLufs = -23.0f;
    s.widenerEnabled = false; // stay tight/mono-ish -- discretion, not spaciousness, at low volume
    s.widenerWidth = 1.0f;
    s.surround3dEnabled = false; // extra ambience would only compete with dialogue clarity here
    s.surround3dIntensity = 1.0f;
    return s;
}

inline AudioDspSettings makePodcastPreset() {
    AudioDspSettings s;
    s.dspEnabled = true;
    s.eqBandGainDb[0] = -4.0f; // cut rumble/handling noise/plosives
    s.eqBandGainDb[2] = 1.5f;  // a little body
    s.eqBandGainDb[3] = 2.5f;  // presence/intelligibility for speech
    s.compressorEnabled = true;
    s.compressorThresholdDb = -22.0f;
    s.compressorRatio = 5.0f; // keep speaker level consistent across a whole episode
    s.limiterEnabled = true;
    s.limiterCeilingDb = -1.0f;
    s.crossoverEnabled = false;
    s.loudnessEnabled = true;
    s.loudnessTargetLufs = -16.0f; // common podcast-platform target
    s.widenerEnabled = false;      // speech should stay mono-compatible/centered
    s.widenerWidth = 1.0f;
    s.surround3dEnabled = false; // ambience would blur speech clarity, the opposite of the goal
    s.surround3dIntensity = 1.0f;
    return s;
}

inline AudioDspSettings makeGamingPreset() {
    AudioDspSettings s;
    s.dspEnabled = true;
    s.eqBandGainDb[3] = 1.0f; // footstep/positional detail
    s.eqBandGainDb[4] = 2.0f; // high-frequency cues (footsteps, reloads, UI)
    s.compressorEnabled = true;
    s.compressorThresholdDb = -20.0f;
    s.compressorRatio = 2.0f; // light -- don't blunt the transients that carry positional cues
    s.limiterEnabled = true;
    s.limiterCeilingDb = -1.0f;
    s.crossoverEnabled = false;
    s.loudnessEnabled = true;
    s.loudnessTargetLufs = -18.0f;
    s.widenerEnabled = true;
    s.widenerWidth = 1.4f; // wider image helps left/right positional awareness
    s.surround3dEnabled = true;
    s.surround3dIntensity = 0.8f; // moderate -- enough spatial cue without smearing directional timing
    return s;
}

inline AudioDspSettings makeLivePreset() {
    AudioDspSettings s;
    s.dspEnabled = true;
    s.eqBandGainDb[0] = 1.5f; // a little low-end warmth
    s.eqBandGainDb[4] = 1.0f; // open, airy top end
    s.compressorEnabled = true;
    s.compressorThresholdDb = -18.0f;
    s.compressorRatio = 1.5f; // light touch -- preserve a live recording's real dynamics
    s.limiterEnabled = true;
    s.limiterCeilingDb = -1.0f;
    s.crossoverEnabled = false;
    s.loudnessEnabled = true;
    s.loudnessTargetLufs = -16.0f;
    s.widenerEnabled = true;
    s.widenerWidth = 1.5f;
    s.surround3dEnabled = true;
    s.surround3dIntensity = 1.3f; // strong -- concert/live recordings benefit most from a "being there" feel
    return s;
}

inline AudioDspSettings makeBassBoostPreset() {
    AudioDspSettings s;
    s.dspEnabled = true;
    s.eqBandGainDb[0] = 8.0f; // strong low-end lift
    s.eqBandGainDb[1] = 2.0f; // a little upper-bass body so it doesn't sound like sub-bass mud
    s.compressorEnabled = true;
    s.compressorThresholdDb = -18.0f;
    s.compressorRatio = 3.0f; // control the boosted low end so it doesn't pump
    s.limiterEnabled = true;
    s.limiterCeilingDb = -1.0f; // essential backstop given an 8dB low-shelf-ish boost
    s.crossoverEnabled = false;
    s.loudnessEnabled = true;
    s.loudnessTargetLufs = -16.0f;
    s.widenerEnabled = false; // keep bass centered/mono-compatible -- wide bass reads weak/phasey
    s.widenerWidth = 1.0f;
    s.surround3dEnabled = false;
    s.surround3dIntensity = 1.0f;
    return s;
}

inline AudioDspSettings makeVocalBoostPreset() {
    AudioDspSettings s;
    s.dspEnabled = true;
    s.eqBandGainDb[0] = -2.0f; // reduce competing low-end mud
    s.eqBandGainDb[2] = 2.0f;  // vocal body
    s.eqBandGainDb[3] = 3.0f;  // vocal presence/clarity
    s.compressorEnabled = true;
    s.compressorThresholdDb = -20.0f;
    s.compressorRatio = 3.0f;
    s.limiterEnabled = true;
    s.limiterCeilingDb = -1.0f;
    s.crossoverEnabled = false;
    s.loudnessEnabled = true;
    s.loudnessTargetLufs = -16.0f;
    s.widenerEnabled = false; // keep the vocal anchored center, not spread wide
    s.widenerWidth = 1.0f;
    s.surround3dEnabled = false;
    s.surround3dIntensity = 1.0f;
    return s;
}

namespace {
inline std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
inline bool containsKeyword(const std::string& haystackLower, const char* keyword) {
    return haystackLower.find(keyword) != std::string::npos;
}
} // namespace

// Maps a free-form genre tag string (e.g. a container's "genre"/TCON
// field) to one of the canned presets above, via simple keyword matching
// -- crude by design (no real genre taxonomy exists across tagging
// conventions), but good enough for the common cases a user would
// actually tag a file with. Case-insensitive. Returns false (leaving
// outSettings untouched) when nothing recognizable matched, so callers
// can fall back to leaving the current settings alone rather than
// forcing an arbitrary default.
inline bool presetForGenreTag(const std::string& genre, AudioDspSettings& outSettings) {
    if (genre.empty()) return false;
    const std::string g = toLowerAscii(genre);

    if (containsKeyword(g, "podcast") || containsKeyword(g, "speech") ||
        containsKeyword(g, "audiobook") || containsKeyword(g, "talk")) {
        outSettings = makePodcastPreset();
        return true;
    }
    if (containsKeyword(g, "soundtrack") || containsKeyword(g, "score") ||
        containsKeyword(g, "film") || containsKeyword(g, "movie")) {
        outSettings = makeCinemaPreset();
        return true;
    }
    if (containsKeyword(g, "electronic") || containsKeyword(g, "dance") ||
        containsKeyword(g, "hip hop") || containsKeyword(g, "hip-hop") ||
        containsKeyword(g, "rap") || containsKeyword(g, "edm") ||
        containsKeyword(g, "dubstep") || containsKeyword(g, "bass")) {
        outSettings = makeBassBoostPreset();
        return true;
    }
    if (containsKeyword(g, "vocal") || containsKeyword(g, "acapella") ||
        containsKeyword(g, "a cappella")) {
        outSettings = makeVocalBoostPreset();
        return true;
    }
    if (containsKeyword(g, "classical") || containsKeyword(g, "jazz") ||
        containsKeyword(g, "acoustic") || containsKeyword(g, "live") ||
        containsKeyword(g, "unplugged")) {
        outSettings = makeLivePreset();
        return true;
    }
    if (containsKeyword(g, "game") || containsKeyword(g, "gaming")) {
        outSettings = makeGamingPreset();
        return true;
    }
    if (containsKeyword(g, "rock") || containsKeyword(g, "pop") ||
        containsKeyword(g, "metal") || containsKeyword(g, "alternative") ||
        containsKeyword(g, "indie") || containsKeyword(g, "punk")) {
        outSettings = makeMusicPreset();
        return true;
    }
    return false;
}

} // namespace naikav::dsp
