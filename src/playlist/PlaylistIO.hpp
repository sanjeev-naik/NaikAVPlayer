#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "playlist/MediaFileFilter.hpp"
#include "playlist/PlaylistItem.hpp"

namespace naikav::playlist {

namespace detail {

inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// A "scheme://" prefix marks a network URL (http://, rtsp://, etc.), never a
// local path -- Windows drive-letter paths ("C:\...") use a single colon and
// no slashes, so this can't misfire on those.
inline bool looksLikeNetworkUrl(const std::string& s) {
    return s.find("://") != std::string::npos;
}

inline bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

} // namespace detail

// Parses an M3U/M3U8 playlist file. Relative entries are resolved against the
// playlist file's own directory. Entries with a "scheme://" prefix are
// skipped -- NaikAVPlayer has no network-stream support. A resolved local
// path that doesn't exist on disk is kept with isValid=false rather than
// dropped, so the caller can surface (and later fix or remove) a broken
// entry instead of silently losing it.
inline std::vector<PlaylistItem> loadM3U(const std::string& m3uPath) {
    std::vector<PlaylistItem> result;
    std::ifstream f(m3uPath);
    if (!f.is_open()) return result;

    std::filesystem::path baseDir = std::filesystem::path(m3uPath).parent_path();
    std::string pendingTitle;
    std::string line;

    while (std::getline(f, line)) {
        std::string trimmed = detail::trim(line);
        if (trimmed.empty()) continue;

        if (trimmed.rfind("#EXTINF:", 0) == 0) {
            size_t comma = trimmed.find(',');
            pendingTitle = (comma != std::string::npos) ? trimmed.substr(comma + 1) : "";
            continue;
        }
        if (trimmed[0] == '#') {
            continue; // #EXTM3U or any other directive/comment
        }

        if (detail::looksLikeNetworkUrl(trimmed)) {
            pendingTitle.clear();
            continue;
        }

        std::filesystem::path entryPath(trimmed);
        std::filesystem::path resolved =
            entryPath.is_absolute() ? entryPath : (baseDir / entryPath);
        std::string resolvedStr = resolved.string();

        PlaylistItem item;
        item.path = resolvedStr;
        item.displayName = !pendingTitle.empty() ? pendingTitle : resolved.filename().string();
        item.kind = classifyMediaKind(resolvedStr);
        item.isValid = detail::fileExists(resolvedStr);
        result.push_back(item);

        pendingTitle.clear();
    }

    return result;
}

inline void saveM3U(const std::string& m3uPath, const std::vector<PlaylistItem>& items) {
    std::ofstream f(m3uPath, std::ios::trunc);
    if (!f.is_open()) return;

    f << "#EXTM3U\n";
    for (const auto& item : items) {
        f << "#EXTINF:-1," << item.displayName << "\n";
        f << item.path << "\n";
    }
}

} // namespace naikav::playlist
