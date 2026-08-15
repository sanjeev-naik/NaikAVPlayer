#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

class FrameExporter {
public:
    struct ExportResult {
        bool success = false;
        std::string filepath;
        std::string errorMessage;
    };

    static ExportResult saveFrameAsPng(
        const AVFrame* srcFrame,
        const std::string& mediaPath,
        double playbackTimestampSeconds,
        const std::string& outputDir = "screenshots"
    ) {
        ExportResult res;
        if (!srcFrame || !srcFrame->data[0] || srcFrame->width <= 0 || srcFrame->height <= 0) {
            res.errorMessage = "No active video frame to capture";
            return res;
        }

        try {
            std::filesystem::create_directories(outputDir);
        } catch (const std::exception& e) {
            res.errorMessage = std::string("Failed to create directory: ") + e.what();
            return res;
        }

        // Determine sanitized base filename from source media
        std::string baseName = "capture";
        if (!mediaPath.empty()) {
            size_t lastSlash = mediaPath.find_last_of("/\\");
            std::string raw = (lastSlash == std::string::npos) ? mediaPath : mediaPath.substr(lastSlash + 1);
            size_t dot = raw.find_last_of('.');
            baseName = (dot == std::string::npos) ? raw : raw.substr(0, dot);
        }
        for (char& c : baseName) {
            if (c == ' ' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '/' || c == '\\') {
                c = '_';
            }
        }

        // Current system date-time string (YYYYMMDD_HHMMSS)
        auto now = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm tmNow{};
#ifdef _WIN32
        localtime_s(&tmNow, &tt);
#else
        localtime_r(&tt, &tmNow);
#endif
        char dateBuf[32];
        std::strftime(dateBuf, sizeof(dateBuf), "%Y%m%d_%H%M%S", &tmNow);

        // Format playback timestamp (e.g. 01m23s or 01h02m03s)
        int totalSec = std::max(0, static_cast<int>(playbackTimestampSeconds));
        int hours = totalSec / 3600;
        int mins = (totalSec % 3600) / 60;
        int secs = totalSec % 60;
        char timeBuf[32];
        if (hours > 0) {
            std::snprintf(timeBuf, sizeof(timeBuf), "%02dh%02dm%02ds", hours, mins, secs);
        } else {
            std::snprintf(timeBuf, sizeof(timeBuf), "%02dm%02ds", mins, secs);
        }

        std::string filename = "NaikAVPlayer_" + baseName + "_" + dateBuf + "_" + timeBuf + ".png";
        std::filesystem::path outPath = std::filesystem::path(outputDir) / filename;

        const int width = srcFrame->width;
        const int height = srcFrame->height;

        // Find PNG encoder in FFmpeg
        const AVCodec* pngCodec = avcodec_find_encoder(AV_CODEC_ID_PNG);
        if (!pngCodec) {
            res.errorMessage = "PNG encoder not found in FFmpeg build";
            return res;
        }

        AVCodecContext* codecCtx = avcodec_alloc_context3(pngCodec);
        if (!codecCtx) {
            res.errorMessage = "Failed to allocate PNG encoder context";
            return res;
        }

        codecCtx->width = width;
        codecCtx->height = height;
        codecCtx->pix_fmt = AV_PIX_FMT_RGB24;
        codecCtx->time_base = AVRational{1, 25};

        if (avcodec_open2(codecCtx, pngCodec, nullptr) < 0) {
            avcodec_free_context(&codecCtx);
            res.errorMessage = "Failed to initialize PNG encoder";
            return res;
        }

        // Allocate RGB frame buffer
        AVFrame* rgbFrame = av_frame_alloc();
        if (!rgbFrame) {
            avcodec_free_context(&codecCtx);
            res.errorMessage = "Failed to allocate RGB frame";
            return res;
        }

        rgbFrame->format = AV_PIX_FMT_RGB24;
        rgbFrame->width = width;
        rgbFrame->height = height;
        if (av_frame_get_buffer(rgbFrame, 0) < 0) {
            av_frame_free(&rgbFrame);
            avcodec_free_context(&codecCtx);
            res.errorMessage = "Failed to allocate RGB frame buffer";
            return res;
        }

        // Convert source planar/packed frame into RGB24
        SwsContext* swsCtx = sws_getContext(
            width, height, static_cast<AVPixelFormat>(srcFrame->format),
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!swsCtx) {
            av_frame_free(&rgbFrame);
            avcodec_free_context(&codecCtx);
            res.errorMessage = "Failed to create color conversion context";
            return res;
        }

        sws_scale(swsCtx, srcFrame->data, srcFrame->linesize, 0, height, rgbFrame->data, rgbFrame->linesize);
        sws_freeContext(swsCtx);

        // Encode RGB frame to PNG packet
        int sendRet = avcodec_send_frame(codecCtx, rgbFrame);
        av_frame_free(&rgbFrame);
        if (sendRet < 0) {
            avcodec_free_context(&codecCtx);
            res.errorMessage = "Failed to encode RGB frame";
            return res;
        }

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            avcodec_free_context(&codecCtx);
            res.errorMessage = "Failed to allocate output packet";
            return res;
        }

        int recvRet = avcodec_receive_packet(codecCtx, pkt);
        if (recvRet < 0) {
            av_packet_free(&pkt);
            avcodec_free_context(&codecCtx);
            res.errorMessage = "Failed to retrieve encoded PNG packet";
            return res;
        }

        // Write output PNG file to disk
        std::ofstream outFile(outPath, std::ios::binary);
        if (!outFile.is_open()) {
            av_packet_free(&pkt);
            avcodec_free_context(&codecCtx);
            res.errorMessage = "Failed to open output file for writing: " + outPath.string();
            return res;
        }

        outFile.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
        outFile.close();

        av_packet_free(&pkt);
        avcodec_free_context(&codecCtx);

        res.success = true;
        res.filepath = outPath.string();
        return res;
    }
};
