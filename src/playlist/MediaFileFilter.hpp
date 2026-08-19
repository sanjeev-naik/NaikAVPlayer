#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

#include "playlist/PlaylistItem.hpp"

namespace naikav::playlist {

// Mirrors the NFD dialog filters in src/app/main.cpp (openNativeFileDialog /
// openNativeAudioDialog) -- kept here as the single source of truth so the
// playlist's "Add Files" dialog, folder scanning, and drag-drop classification
// all agree on what NaikAVPlayer considers a supported media file.
inline constexpr std::array<std::string_view, 5> kVideoExtensions = {
    "mp4", "mkv", "avi", "mov", "webm"
};

inline constexpr std::array<std::string_view, 10> kAudioExtensions = {
    "m4a", "aac", "ac3", "mp3", "wav", "flac", "ogg", "opus", "wma", "mka"
};

inline std::string toLowerExtension(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return "";
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

inline MediaKind classifyMediaKind(const std::string& path) {
    std::string ext = toLowerExtension(path);
    if (ext.empty()) {
        return MediaKind::Unknown;
    }
    if (std::any_of(kVideoExtensions.begin(), kVideoExtensions.end(),
                    [&ext](std::string_view v) { return ext == v; })) {
        return MediaKind::Video;
    }
    if (std::any_of(kAudioExtensions.begin(), kAudioExtensions.end(),
                    [&ext](std::string_view a) { return ext == a; })) {
        return MediaKind::Audio;
    }
    return MediaKind::Unknown;
}

inline bool isSupportedMediaFile(const std::string& path) {
    return classifyMediaKind(path) != MediaKind::Unknown;
}

// "ext1,ext2,..." filter string for NFD dialogs (video extensions first,
// then audio), e.g. for the playlist's multi-select "Add Files" dialog.
inline std::string combinedExtensionFilterString() {
    std::string result;
    for (auto v : kVideoExtensions) {
        if (!result.empty()) result += ",";
        result += v;
    }
    for (auto a : kAudioExtensions) {
        result += ",";
        result += a;
    }
    return result;
}

} // namespace naikav::playlist
