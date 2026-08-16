#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cctype>

namespace naikav {
namespace subtitle {

struct SubtitleEvent {
    double startPts = 0.0; // Start timestamp in seconds
    double endPts = 0.0;   // End timestamp in seconds
    std::string text;      // Cleaned, human-readable display text
    std::string rawAss;    // Original raw ASS/SSA markup (if available)

    bool isActive(double pts) const {
        return pts >= startPts && pts <= endPts;
    }
};

struct SubtitleTrackInfo {
    int id = -1;                // Stream index (>= 0 for embedded, or -1 for external)
    std::string title;         // Track title/name (e.g., "English [SDH]", "Director Commentary")
    std::string language;      // ISO 639-1/2 language code (e.g., "eng", "jpn", "spa")
    std::string codecName;     // Codec name (e.g., "subrip", "ass", "webvtt", "mov_text")
    bool isExternal = false;   // True if loaded from a standalone external file (.srt, .vtt, .ass)
    std::string sourcePath;    // Filepath if external
};

// Strips ASS override blocks like {\pos(100,200)}, {\an8}, {\b1}, {\c&Hffffff&}
// Normalizes \N, \n, and \h into standard whitespace/newlines
// Strips basic HTML formatting tags (<b>, <i>, <u>, <font...>, etc.)
inline std::string sanitizeSubtitleText(const std::string& input) {
    std::string text = input;

    // Check if input is an ASS dialogue line (e.g., from FFmpeg's rect->ass or .ass files)
    // "Dialogue: Marked=0,0:00:01.00,0:00:03.50,Default,,0,0,0,,Text" -> 9 commas
    // "0,0,Default,,0,0,0,,Text" -> 8 commas
    if (text.rfind("Dialogue:", 0) == 0) {
        size_t pos = 0;
        int commaCount = 0;
        while (pos < text.size() && commaCount < 9) {
            if (text[pos] == ',') {
                commaCount++;
            }
            pos++;
        }
        if (commaCount == 9) {
            text = text.substr(pos);
        }
    } else {
        int commaCount = 0;
        size_t pos = 0;
        while (pos < text.size() && text[pos] != '\n' && commaCount < 8) {
            if (text[pos] == ',') {
                commaCount++;
            }
            pos++;
        }
        if (commaCount == 8) {
            text = text.substr(pos);
        }
    }

    std::string result;
    result.reserve(text.size());

    bool insideBrace = false;
    bool insideAngle = false;

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        // Handle ASS escape sequences like \N (forced newline), \n (soft newline), \h (hard space)
        if (c == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == 'N' || next == 'n') {
                if (!insideBrace && !insideAngle) {
                    result.push_back('\n');
                }
                ++i;
                continue;
            } else if (next == 'h' || next == 'H') {
                if (!insideBrace && !insideAngle) {
                    result.push_back(' ');
                }
                ++i;
                continue;
            }
        }

        // ASS override tag {...}
        if (c == '{') {
            insideBrace = true;
            continue;
        } else if (c == '}' && insideBrace) {
            insideBrace = false;
            continue;
        }

        // HTML tag <...>
        if (c == '<') {
            insideAngle = true;
            continue;
        } else if (c == '>' && insideAngle) {
            insideAngle = false;
            continue;
        }

        if (!insideBrace && !insideAngle) {
            if (c != '\r') {
                result.push_back(c);
            }
        }
    }

    // Trim leading and trailing whitespace
    while (!result.empty() && (result.back() == ' ' || result.back() == '\n' || result.back() == '\t')) {
        result.pop_back();
    }
    size_t start = 0;
    while (start < result.size() && (result[start] == ' ' || result[start] == '\n' || result[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        result = result.substr(start);
    }

    return result;
}

} // namespace subtitle
} // namespace naikav
