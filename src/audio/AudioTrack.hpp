#pragma once

#include <string>
#include <cstdint>

namespace naikav {
namespace audio {

struct AudioTrackInfo {
    int id = -1;                // Stream index in format context (>= 0 for embedded, -2 for external, -1 for disabled)
    std::string title;          // Track title or description (e.g., "Main Audio", "Director's Commentary")
    std::string language;       // ISO 639-1/2 language code (e.g., "eng", "jpn", "hin", "und")
    std::string codecName;      // Codec name (e.g., "aac", "ac3", "dts", "flac", "mp3", "opus")
    int channels = 0;           // Number of audio channels (e.g., 2, 6, 8)
    std::string channelLayout;  // Channel layout name (e.g., "stereo", "5.1(side)", "7.1")
    int sampleRate = 0;         // Sample rate in Hz (e.g., 44100, 48000, 96000)
    int64_t bitRate = 0;        // Bitrate in bps
    bool isDefault = false;     // True if disposition has AV_DISPOSITION_DEFAULT
    bool isExternal = false;    // True if loaded from a standalone external file
    std::string sourcePath;     // Filepath if external
};

} // namespace audio
} // namespace naikav
