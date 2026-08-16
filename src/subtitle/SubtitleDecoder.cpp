#include "subtitle/SubtitleDecoder.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace naikav {
namespace subtitle {

SubtitleDecoder::SubtitleDecoder() {
}

SubtitleDecoder::~SubtitleDecoder() {
    reset();
}

void SubtitleDecoder::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    m_events.clear();
    m_isExternal = false;
    m_externalFilePath.clear();
    m_timeBase = {0, 1};
    m_startTime = 0;
}

void SubtitleDecoder::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_codecCtx) {
        avcodec_flush_buffers(m_codecCtx);
    }
    // For embedded live streaming, we preserve events that might still be useful
    // or allow new packets to populate it.
}

size_t SubtitleDecoder::getEventCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_events.size();
}

std::string SubtitleDecoder::extractTextFromAss(const char* assLine) {
    if (!assLine) return "";
    const char* str = assLine;

    // Check if line starts with Dialogue:
    if (std::strncmp(str, "Dialogue:", 9) == 0) {
        str += 9;
        // Skip 9 commas to reach the text payload
        int commaCount = 0;
        while (*str && commaCount < 9) {
            if (*str == ',') {
                commaCount++;
            }
            str++;
        }
    } else {
        // Raw ASS format from FFmpeg: 8 commas before text
        int commaCount = 0;
        const char* p = str;
        while (*p && *p != '\n' && commaCount < 8) {
            if (*p == ',') {
                commaCount++;
            }
            p++;
        }
        if (commaCount == 8) {
            str = p;
        }
    }
    return sanitizeSubtitleText(str);
}

static bool parseTimestamp(const std::string& str, double& outSeconds) {
    // Finds hour, minute, and second.fraction
    // Formats: H:MM:SS.mmm, HH:MM:SS,mmm, MM:SS.mmm, etc.
    size_t colon1 = str.find(':');
    if (colon1 == std::string::npos) return false;

    size_t colon2 = str.find(':', colon1 + 1);
    if (colon2 != std::string::npos) {
        // H:MM:SS.frac or HH:MM:SS,frac
        try {
            int h = std::stoi(str.substr(0, colon1));
            int m = std::stoi(str.substr(colon1 + 1, colon2 - colon1 - 1));
            std::string secStr = str.substr(colon2 + 1);
            size_t comma = secStr.find(',');
            if (comma != std::string::npos) {
                secStr[comma] = '.';
            }
            double s = std::stod(secStr);
            outSeconds = h * 3600.0 + m * 60.0 + s;
            return true;
        } catch (...) {
            return false;
        }
    } else {
        // MM:SS.frac or MM:SS,frac
        try {
            int m = std::stoi(str.substr(0, colon1));
            std::string secStr = str.substr(colon1 + 1);
            size_t comma = secStr.find(',');
            if (comma != std::string::npos) {
                secStr[comma] = '.';
            }
            double s = std::stod(secStr);
            outSeconds = m * 60.0 + s;
            return true;
        } catch (...) {
            return false;
        }
    }
}

bool SubtitleDecoder::init(AVCodecParameters* codecParams, AVRational timeBase, int64_t startTime) {
    reset();

    if (!codecParams) return false;

    const AVCodec* decoder = avcodec_find_decoder(codecParams->codec_id);
    if (!decoder) {
        std::cerr << "Warning: No subtitle decoder found for codec ID " << codecParams->codec_id << std::endl;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(decoder);
    if (!m_codecCtx) return false;

    if (avcodec_parameters_to_context(m_codecCtx, codecParams) < 0) {
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    if (avcodec_open2(m_codecCtx, decoder, nullptr) < 0) {
        std::cerr << "Warning: Could not open subtitle codec context" << std::endl;
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_timeBase = timeBase;
    m_startTime = startTime;
    m_isExternal = false;
    return true;
}

void SubtitleDecoder::processPacket(AVPacket* pkt) {
    if (!pkt || !m_codecCtx) return;

    if (m_seekGeneration) {
        uint64_t currentGen = m_seekGeneration->load(std::memory_order_relaxed);
        uint64_t pktGen = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pkt->opaque));
        if (pktGen != currentGen) {
            return; // Drop packet from pre-seek stream
        }
    }

    AVSubtitle sub;
    std::memset(&sub, 0, sizeof(sub));
    int gotSub = 0;

    int ret = avcodec_decode_subtitle2(m_codecCtx, &sub, &gotSub, pkt);
    if (ret < 0 || !gotSub) {
        return;
    }

    double basePts = 0.0;
    if (pkt->pts != AV_NOPTS_VALUE) {
        basePts = (pkt->pts - m_startTime) * av_q2d(m_timeBase);
    } else if (pkt->dts != AV_NOPTS_VALUE) {
        basePts = (pkt->dts - m_startTime) * av_q2d(m_timeBase);
    }

    double startPts = basePts;
    if (sub.start_display_time > 0) {
        startPts += sub.start_display_time / 1000.0;
    }

    double durationSec = 3.5;
    if (sub.end_display_time > 0 && sub.end_display_time != UINT32_MAX) {
        durationSec = sub.end_display_time / 1000.0;
    } else if (pkt->duration > 0) {
        durationSec = pkt->duration * av_q2d(m_timeBase);
    }
    double endPts = startPts + durationSec;

    std::string combinedText;
    for (unsigned int i = 0; i < sub.num_rects; ++i) {
        AVSubtitleRect* rect = sub.rects[i];
        if (!rect) continue;

        std::string piece;
        if (rect->type == SUBTITLE_ASS && rect->ass) {
            // Check if ASS line has explicit start/end timestamps
            const char* assStr = rect->ass;
            if (std::strncmp(assStr, "Dialogue:", 9) == 0) {
                // Dialogue: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
                std::string line(assStr);
                size_t p = 9;
                std::vector<std::string> fields;
                std::string curFld;
                while (p < line.size() && fields.size() < 9) {
                    if (line[p] == ',') {
                        fields.push_back(curFld);
                        curFld.clear();
                    } else {
                        curFld.push_back(line[p]);
                    }
                    p++;
                }
                if (fields.size() >= 3) {
                    double assStart = 0.0, assEnd = 0.0;
                    if (parseTimestamp(fields[1], assStart) && parseTimestamp(fields[2], assEnd)) {
                        startPts = assStart;
                        endPts = assEnd;
                    }
                }
            }
            piece = extractTextFromAss(rect->ass);
        } else if (rect->type == SUBTITLE_TEXT && rect->text) {
            piece = sanitizeSubtitleText(rect->text);
        }

        if (!piece.empty()) {
            if (!combinedText.empty()) combinedText += "\n";
            combinedText += piece;
        }
    }

    avsubtitle_free(&sub);

    if (!combinedText.empty()) {
        SubtitleEvent ev;
        ev.startPts = startPts;
        ev.endPts = endPts;
        ev.text = combinedText;

        std::lock_guard<std::mutex> lock(m_mutex);
        // Avoid duplicate events at same start time
        bool duplicate = false;
        for (auto& existing : m_events) {
            if (std::abs(existing.startPts - ev.startPts) < 0.05 && existing.text == ev.text) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            m_events.push_back(ev);
            // Keep events sorted by startPts
            std::sort(m_events.begin(), m_events.end(), [](const SubtitleEvent& a, const SubtitleEvent& b) {
                return a.startPts < b.startPts;
            });
        }
    }
}

bool SubtitleDecoder::loadExternalFile(const std::string& filepath) {
    reset();

    // 1. Fast direct parser for common text subtitle formats (.srt, .vtt, .ass, .ssa)
    std::string ext = "";
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos);
        for (char& c : ext) c = static_cast<char>(std::tolower(c));
    }

    if (ext == ".srt" || ext == ".vtt") {
        parseSrtVttFallback(filepath);
        if (!m_events.empty()) {
            std::sort(m_events.begin(), m_events.end(), [](const SubtitleEvent& a, const SubtitleEvent& b) {
                return a.startPts < b.startPts;
            });
            m_isExternal = true;
            m_externalFilePath = filepath;
            return true;
        }
    } else if (ext == ".ass" || ext == ".ssa") {
        parseAssSsaFallback(filepath);
        if (!m_events.empty()) {
            std::sort(m_events.begin(), m_events.end(), [](const SubtitleEvent& a, const SubtitleEvent& b) {
                return a.startPts < b.startPts;
            });
            m_isExternal = true;
            m_externalFilePath = filepath;
            return true;
        }
    }

    // 2. FFmpeg demuxer / decoder fallback for other external subtitle formats (.sub, etc.)
    AVFormatContext* fmtCtx = nullptr;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file,pipe", 0);

    int ret = avformat_open_input(&fmtCtx, filepath.c_str(), nullptr, &options);
    av_dict_free(&options);

    bool loadedViaFfmpeg = false;
    if (ret >= 0 && avformat_find_stream_info(fmtCtx, nullptr) >= 0) {
        int streamIdx = -1;
        for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                streamIdx = static_cast<int>(i);
                break;
            }
        }

        if (streamIdx >= 0) {
            AVCodecParameters* codecParams = fmtCtx->streams[streamIdx]->codecpar;
            const AVCodec* decoder = avcodec_find_decoder(codecParams->codec_id);
            if (decoder) {
                AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
                if (codecCtx && avcodec_parameters_to_context(codecCtx, codecParams) >= 0 &&
                    avcodec_open2(codecCtx, decoder, nullptr) >= 0) {

                    AVRational tb = fmtCtx->streams[streamIdx]->time_base;
                    int64_t st = fmtCtx->streams[streamIdx]->start_time;
                    if (st == AV_NOPTS_VALUE) st = 0;

                    AVPacket* pkt = av_packet_alloc();
                    while (av_read_frame(fmtCtx, pkt) >= 0) {
                        if (pkt->stream_index == streamIdx) {
                            AVSubtitle sub;
                            std::memset(&sub, 0, sizeof(sub));
                            int gotSub = 0;
                            if (avcodec_decode_subtitle2(codecCtx, &sub, &gotSub, pkt) >= 0 && gotSub) {
                                double basePts = 0.0;
                                if (pkt->pts != AV_NOPTS_VALUE) {
                                    basePts = (pkt->pts - st) * av_q2d(tb);
                                } else if (pkt->dts != AV_NOPTS_VALUE) {
                                    basePts = (pkt->dts - st) * av_q2d(tb);
                                }

                                double startPts = basePts + (sub.start_display_time / 1000.0);
                                double dur = 3.5;
                                if (sub.end_display_time > 0 && sub.end_display_time != UINT32_MAX) {
                                    dur = sub.end_display_time / 1000.0;
                                } else if (pkt->duration > 0) {
                                    dur = pkt->duration * av_q2d(tb);
                                }
                                double endPts = startPts + dur;

                                std::string combinedText;
                                for (unsigned int j = 0; j < sub.num_rects; ++j) {
                                    AVSubtitleRect* rect = sub.rects[j];
                                    if (!rect) continue;

                                    std::string piece;
                                    if (rect->type == SUBTITLE_ASS && rect->ass) {
                                        piece = extractTextFromAss(rect->ass);
                                    } else if (rect->type == SUBTITLE_TEXT && rect->text) {
                                        piece = sanitizeSubtitleText(rect->text);
                                    }

                                    if (!piece.empty()) {
                                        if (!combinedText.empty()) combinedText += "\n";
                                        combinedText += piece;
                                    }
                                }
                                avsubtitle_free(&sub);

                                if (!combinedText.empty()) {
                                    SubtitleEvent ev;
                                    ev.startPts = startPts;
                                    ev.endPts = endPts;
                                    ev.text = combinedText;
                                    m_events.push_back(ev);
                                }
                            }
                        }
                        av_packet_unref(pkt);
                    }
                    av_packet_free(&pkt);
                    avcodec_free_context(&codecCtx);
                    loadedViaFfmpeg = !m_events.empty();
                }
            }
        }
        avformat_close_input(&fmtCtx);
    }

    if (!loadedViaFfmpeg || m_events.empty()) {
        parseSrtVttFallback(filepath);
        if (m_events.empty()) {
            parseAssSsaFallback(filepath);
        }
    }

    if (!m_events.empty()) {
        std::sort(m_events.begin(), m_events.end(), [](const SubtitleEvent& a, const SubtitleEvent& b) {
            return a.startPts < b.startPts;
        });
        m_isExternal = true;
        m_externalFilePath = filepath;
        return true;
    }

    return false;
}

void SubtitleDecoder::parseSrtVttFallback(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;

    std::string line;
    SubtitleEvent currentEvent;
    bool inEvent = false;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Check for timestamp separator "-->"
        size_t arrowPos = line.find("-->");
        if (arrowPos != std::string::npos) {
            if (inEvent && !currentEvent.text.empty()) {
                currentEvent.text = sanitizeSubtitleText(currentEvent.text);
                if (!currentEvent.text.empty()) {
                    m_events.push_back(currentEvent);
                }
                currentEvent = SubtitleEvent();
            }

            std::string startStr = line.substr(0, arrowPos);
            std::string endStr = line.substr(arrowPos + 3);

            auto trim = [](std::string& s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            };
            trim(startStr);
            trim(endStr);

            size_t spacePos = endStr.find(' ');
            if (spacePos != std::string::npos) {
                endStr = endStr.substr(0, spacePos);
            }

            double startSec = 0.0, endSec = 0.0;
            if (parseTimestamp(startStr, startSec) && parseTimestamp(endStr, endSec)) {
                currentEvent.startPts = startSec;
                currentEvent.endPts = endSec;
                currentEvent.text.clear();
                inEvent = true;
            }
        } else if (inEvent) {
            if (line.empty()) {
                currentEvent.text = sanitizeSubtitleText(currentEvent.text);
                if (!currentEvent.text.empty()) {
                    m_events.push_back(currentEvent);
                }
                currentEvent = SubtitleEvent();
                inEvent = false;
            } else {
                if (!currentEvent.text.empty()) {
                    currentEvent.text += "\n";
                }
                currentEvent.text += line;
            }
        }
    }

    if (inEvent && !currentEvent.text.empty()) {
        currentEvent.text = sanitizeSubtitleText(currentEvent.text);
        if (!currentEvent.text.empty()) {
            m_events.push_back(currentEvent);
        }
    }
}

void SubtitleDecoder::parseAssSsaFallback(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Check if line starts with Dialogue:
        if (line.rfind("Dialogue:", 0) == 0) {
            // Dialogue: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            size_t p = 9;
            std::vector<std::string> fields;
            std::string curFld;
            while (p < line.size() && fields.size() < 9) {
                if (line[p] == ',') {
                    fields.push_back(curFld);
                    curFld.clear();
                } else {
                    curFld.push_back(line[p]);
                }
                p++;
            }
            std::string textPayload = (p < line.size()) ? line.substr(p) : curFld;

            if (fields.size() >= 3) {
                std::string startStr = fields[1];
                std::string endStr = fields[2];

                auto trim = [](std::string& s) {
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
                };
                trim(startStr);
                trim(endStr);

                double startSec = 0.0, endSec = 0.0;
                if (parseTimestamp(startStr, startSec) && parseTimestamp(endStr, endSec)) {
                    std::string cleanText = sanitizeSubtitleText(textPayload);
                    if (!cleanText.empty()) {
                        SubtitleEvent ev;
                        ev.startPts = startSec;
                        ev.endPts = endSec;
                        ev.text = cleanText;
                        m_events.push_back(ev);
                    }
                }
            }
        }
    }
}

std::string SubtitleDecoder::getActiveSubtitleText(double currentPts, double delaySeconds) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Standard convention: positive delay delays subtitle presentation, negative advances it
    double targetPts = currentPts - delaySeconds;
    std::string result;

    for (const auto& ev : m_events) {
        if (ev.isActive(targetPts)) {
            if (!result.empty()) {
                result += "\n";
            }
            result += ev.text;
        }
    }
    return result;
}

std::vector<SubtitleEvent> SubtitleDecoder::getActiveEvents(double currentPts, double delaySeconds) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    double targetPts = currentPts - delaySeconds;
    std::vector<SubtitleEvent> result;

    for (const auto& ev : m_events) {
        if (ev.isActive(targetPts)) {
            result.push_back(ev);
        }
    }
    return result;
}

} // namespace subtitle
} // namespace naikav
