#pragma once

extern "C" {
#include <libavutil/dict.h>
}

#include <cstdlib>
#include <cstring>

namespace naikav::dsp {

// Reads a container/tag-embedded loudness reference (ReplayGain or EBU
// R128 gain tags) and converts it to an equivalent whole-file integrated
// LUFS figure, suitable for feeding straight into
// LoudnessNormalizer::primeWithPrescannedLufs() -- the same two-pass
// entry point naikav::dsp::prescanIntegratedLufs() uses, just sourced
// from a tag instead of a decode pass. This is strictly faster (no decode
// needed at all) and, when present, at least as accurate for
// gapless-correct level matching, since it's exactly what the encoder (or
// a tagging tool like `mp3gain`/`rsgain`/`loudgain`) already measured
// against the whole file.
//
// Checked in priority order (most spec-precise first):
//   1. R128_TRACK_GAIN     -- Ogg/Opus EBU R128 tag: integer, Q7.8 fixed
//      point (256 units = 1 dB), the gain needed to reach EBU R128's
//      -23 LUFS reference.
//   2. REPLAYGAIN_TRACK_GAIN -- classic ReplayGain tag: a string like
//      "-6.50 dB", referenced to ReplayGain 2.0's -18 LUFS convention
//      (the one modern taggers/players use; RG1.0's 89dB SPL reference
//      is close enough in practice that this project doesn't special-case
//      it).
// Album-level tags (R128_ALBUM_GAIN / REPLAYGAIN_ALBUM_GAIN) are checked
// as a fallback when no track-level tag exists, since this project has
// no album/playlist concept to prefer one over the other by context.
//
// FFmpeg's av_dict_get() is case-insensitive by default (no
// AV_DICT_MATCH_CASE flag), so this matches tags regardless of the
// casing a given container/tagger used.
//
// Returns true and fills outEquivalentLufs if a usable tag was found;
// false (leaving outEquivalentLufs untouched) otherwise.
inline bool readTaggedLoudnessAsLufs(const AVDictionary* formatMetadata,
                                      const AVDictionary* streamMetadata,
                                      double& outEquivalentLufs) {
    auto tryDict = [&](const AVDictionary* dict) -> bool {
        if (!dict) return false;

        if (const AVDictionaryEntry* e = av_dict_get(dict, "R128_TRACK_GAIN", nullptr, 0)) {
            char* end = nullptr;
            long q78 = std::strtol(e->value, &end, 10);
            if (end != e->value) {
                double gainDb = static_cast<double>(q78) / 256.0;
                outEquivalentLufs = -23.0 - gainDb; // R128 tags target -23 LUFS
                return true;
            }
        }
        if (const AVDictionaryEntry* e = av_dict_get(dict, "REPLAYGAIN_TRACK_GAIN", nullptr, 0)) {
            char* end = nullptr;
            double gainDb = std::strtod(e->value, &end);
            if (end != e->value) {
                outEquivalentLufs = -18.0 - gainDb; // RG2.0 convention: -18 LUFS reference
                return true;
            }
        }
        if (const AVDictionaryEntry* e = av_dict_get(dict, "R128_ALBUM_GAIN", nullptr, 0)) {
            char* end = nullptr;
            long q78 = std::strtol(e->value, &end, 10);
            if (end != e->value) {
                double gainDb = static_cast<double>(q78) / 256.0;
                outEquivalentLufs = -23.0 - gainDb;
                return true;
            }
        }
        if (const AVDictionaryEntry* e = av_dict_get(dict, "REPLAYGAIN_ALBUM_GAIN", nullptr, 0)) {
            char* end = nullptr;
            double gainDb = std::strtod(e->value, &end);
            if (end != e->value) {
                outEquivalentLufs = -18.0 - gainDb;
                return true;
            }
        }
        return false;
    };

    // Stream-level metadata is checked first: some containers (e.g. FLAC
    // muxed with per-stream Vorbis comments) attach tags there rather
    // than at the format level.
    return tryDict(streamMetadata) || tryDict(formatMetadata);
}

} // namespace naikav::dsp
