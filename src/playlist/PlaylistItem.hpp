#pragma once

#include <cstdint>
#include <string>

namespace naikav::playlist {

enum class MediaKind {
    Video,
    Audio,
    Unknown
};

struct PlaylistItem {
    // 0 means "unassigned" -- Playlist assigns a stable non-zero id when an
    // item is added, used to track the current item across reorders/removals
    // without relying on index arithmetic. See Playlist::indexOfId().
    uint64_t id = 0;
    std::string path;
    std::string displayName;
    MediaKind kind = MediaKind::Unknown;
    // False for an entry whose file couldn't be found on disk (e.g. loaded
    // from an M3U pointing at a moved/deleted file). Kept in the list rather
    // than dropped so the user can see and fix/remove it; Playlist::next()/
    // previous() skip over invalid entries automatically.
    bool isValid = true;
};

} // namespace naikav::playlist
