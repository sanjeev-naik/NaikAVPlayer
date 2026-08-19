#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "playlist/MediaFileFilter.hpp"
#include "playlist/PlaylistIO.hpp"
#include "playlist/PlaylistItem.hpp"

namespace naikav::playlist {

enum class RepeatMode {
    Off,
    All,
    One
};

// Pure in-memory playlist model: no SDL/FFmpeg/ImGui dependency, main-thread
// only (mirrors the header-only convention used by src/audio/dsp/* and
// src/core/ThreadSafeQueue.hpp for logic with no such dependency).
//
// The "current item" is tracked by a stable id (assigned on insert), not a
// raw index, so it survives reordering (move()) and removal of other items
// without any index-arithmetic bookkeeping.
class Playlist {
public:
    PlaylistItem add(const std::string& path) {
        PlaylistItem item = makeItem(path);
        m_items.push_back(item);
        regenerateShuffleOrderIfActive();
        return item;
    }

    void addMany(const std::vector<std::string>& paths) {
        std::transform(paths.begin(), paths.end(), std::back_inserter(m_items),
                       [this](const std::string& p) { return makeItem(p); });
        regenerateShuffleOrderIfActive();
    }

    // Non-recursive scan of a directory for supported media files, appended
    // in alphabetical order. Returns the number of files added (0 if the
    // directory can't be opened).
    size_t addFolder(const std::string& dirPath) {
        std::vector<std::string> found;
        std::error_code ec;
        std::filesystem::directory_iterator it(dirPath, ec);
        if (ec) return 0;
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || fileEc) continue;
            std::string p = it->path().string();
            if (isSupportedMediaFile(p)) {
                found.push_back(p);
            }
        }
        std::sort(found.begin(), found.end());
        addMany(found);
        return found.size();
    }

    // index is a position in items() (display order).
    bool removeAt(int index) {
        if (index < 0 || static_cast<size_t>(index) >= m_items.size()) return false;
        bool wasCurrent = (m_items[static_cast<size_t>(index)].id == m_currentId);
        m_items.erase(m_items.begin() + index);
        if (wasCurrent) {
            // Whatever now occupies this slot was "the next item" before the
            // erase (or, if this was the last slot, the new last item) --
            // selecting it is exactly "advance to next remaining item".
            if (m_items.empty()) {
                m_currentId = kNoId;
            } else {
                size_t newIndex = std::min(static_cast<size_t>(index), m_items.size() - 1);
                m_currentId = m_items[newIndex].id;
            }
        }
        regenerateShuffleOrderIfActive();
        return true;
    }

    bool move(int from, int to) {
        if (from < 0 || to < 0 || static_cast<size_t>(from) >= m_items.size() ||
            static_cast<size_t>(to) >= m_items.size() || from == to) {
            return false;
        }
        PlaylistItem item = m_items[static_cast<size_t>(from)];
        m_items.erase(m_items.begin() + from);
        m_items.insert(m_items.begin() + to, item);
        regenerateShuffleOrderIfActive();
        return true; // m_currentId is untouched -- identity survives the move.
    }

    void clear() {
        m_items.clear();
        m_currentId = kNoId;
        m_shuffleOrder.clear();
    }

    size_t size() const { return m_items.size(); }
    bool empty() const { return m_items.empty(); }
    const std::vector<PlaylistItem>& items() const { return m_items; }

    int getCurrentIndex() const { return indexOfId(m_currentId); }

    std::optional<PlaylistItem> currentItem() const {
        int idx = getCurrentIndex();
        if (idx < 0) return std::nullopt;
        return m_items[static_cast<size_t>(idx)];
    }

    // Sets the current item by display-order index. Pure bookkeeping -- does
    // not open/play anything.
    bool setCurrentIndex(int index) {
        if (index < 0 || static_cast<size_t>(index) >= m_items.size()) {
            m_currentId = kNoId;
            return false;
        }
        m_currentId = m_items[static_cast<size_t>(index)].id;
        return true;
    }

    RepeatMode getRepeatMode() const { return m_repeatMode; }
    void setRepeatMode(RepeatMode mode) { m_repeatMode = mode; }

    bool isShuffle() const { return m_shuffle; }
    void setShuffle(bool enabled) {
        m_shuffle = enabled;
        if (m_shuffle) {
            regenerateShuffleOrder();
        }
    }

    std::optional<PlaylistItem> next() { return step(+1); }
    std::optional<PlaylistItem> previous() { return step(-1); }

    // Replaces the playlist contents with an M3U/M3U8 file's entries.
    // Returns false (leaving the playlist untouched) if the file couldn't be
    // read or was empty.
    bool loadM3U(const std::string& path) {
        std::vector<PlaylistItem> loaded = naikav::playlist::loadM3U(path);
        if (loaded.empty()) return false;
        m_items.clear();
        m_currentId = kNoId;
        for (auto& item : loaded) {
            item.id = m_nextId++;
            m_items.push_back(item);
        }
        regenerateShuffleOrderIfActive();
        return true;
    }

    void saveM3U(const std::string& path) const {
        naikav::playlist::saveM3U(path, m_items);
    }

private:
    static constexpr uint64_t kNoId = 0;

    std::vector<PlaylistItem> m_items;
    uint64_t m_nextId = 1;
    uint64_t m_currentId = kNoId;
    RepeatMode m_repeatMode = RepeatMode::Off;
    bool m_shuffle = false;
    std::vector<int> m_shuffleOrder; // permutation of indices into m_items

    PlaylistItem makeItem(const std::string& path) {
        PlaylistItem item;
        item.id = m_nextId++;
        item.path = path;
        item.displayName = std::filesystem::path(path).filename().string();
        item.kind = classifyMediaKind(path);
        std::error_code ec;
        item.isValid = std::filesystem::exists(path, ec) && !ec;
        return item;
    }

    int indexOfId(uint64_t id) const {
        if (id == kNoId) return -1;
        auto it = std::find_if(m_items.begin(), m_items.end(),
                               [id](const PlaylistItem& item) { return item.id == id; });
        return (it != m_items.end()) ? static_cast<int>(std::distance(m_items.begin(), it)) : -1;
    }

    void regenerateShuffleOrderIfActive() {
        if (m_shuffle) regenerateShuffleOrder();
    }

    void regenerateShuffleOrder() {
        m_shuffleOrder.resize(m_items.size());
        for (size_t i = 0; i < m_shuffleOrder.size(); ++i) {
            m_shuffleOrder[i] = static_cast<int>(i);
        }
        static std::mt19937 rng{std::random_device{}()};
        std::shuffle(m_shuffleOrder.begin(), m_shuffleOrder.end(), rng);
    }

    // Position of the current item within the active order (shuffle order if
    // shuffle is on, display order otherwise). -1 if nothing is current.
    int currentPositionInOrder() const {
        int idx = getCurrentIndex();
        if (idx < 0) return -1;
        if (!m_shuffle) return idx;
        auto it = std::find(m_shuffleOrder.begin(), m_shuffleOrder.end(), idx);
        return (it != m_shuffleOrder.end()) ? static_cast<int>(std::distance(m_shuffleOrder.begin(), it)) : -1;
    }

    uint64_t idAtOrderPosition(size_t pos) const {
        size_t itemIndex = m_shuffle ? static_cast<size_t>(m_shuffleOrder[pos]) : pos;
        return m_items[itemIndex].id;
    }

    // direction: +1 for next(), -1 for previous().
    std::optional<PlaylistItem> step(int direction) {
        if (m_items.empty()) return std::nullopt;
        size_t n = m_items.size();

        if (m_repeatMode == RepeatMode::One) {
            int idx = getCurrentIndex();
            if (idx < 0) idx = 0; // nothing selected yet -- start at the first item
            if (m_items[static_cast<size_t>(idx)].isValid) {
                m_currentId = m_items[static_cast<size_t>(idx)].id;
                return m_items[static_cast<size_t>(idx)];
            }
            // The one item being repeated is invalid -- fall through to
            // normal stepping so playback doesn't get stuck forever.
        }

        // Try up to n steps looking for a valid item, so a list containing
        // isValid=false entries is tolerated instead of infinite-looping.
        for (size_t attempt = 0; attempt < n; ++attempt) {
            int curPos = currentPositionInOrder();
            int nextPos;
            if (curPos < 0) {
                // Nothing currently selected: start playback at a sensible
                // end of the list rather than treating this as "past the
                // end" of a repeat-off list.
                nextPos = (direction > 0) ? 0 : static_cast<int>(n) - 1;
            } else {
                nextPos = curPos + direction;
                if (nextPos < 0 || nextPos >= static_cast<int>(n)) {
                    if (m_repeatMode == RepeatMode::Off) {
                        return std::nullopt;
                    }
                    nextPos = (nextPos < 0) ? static_cast<int>(n) - 1 : 0;
                }
            }

            uint64_t candidateId = idAtOrderPosition(static_cast<size_t>(nextPos));
            m_currentId = candidateId;
            int idx = indexOfId(candidateId);
            if (idx >= 0 && m_items[static_cast<size_t>(idx)].isValid) {
                return m_items[static_cast<size_t>(idx)];
            }
            // Invalid -- loop again, stepping from this new position.
        }
        return std::nullopt; // every item invalid
    }
};

} // namespace naikav::playlist
