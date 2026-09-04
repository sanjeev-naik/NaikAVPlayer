#pragma once

// Feature detection for the FFmpeg APIs this player uses that are newer
// than the oldest FFmpeg it is expected to build against.
//
// Two very different FFmpegs are in play. On Windows and Linux x86_64,
// CMake downloads a current build (n8.x: libavutil 60, libavcodec 62,
// libswscale 9) and everything here is available. On Linux **ARM64** the
// build deliberately links the distro's system FFmpeg instead, because
// that is what carries the V4L2 M2M hardware decoding a Raspberry Pi
// needs -- and a Pi OS or Ubuntu of that generation can be as old as
// FFmpeg 4.4 (libavutil 56, libavcodec 58, libswscale 5).
//
// Everything guarded below degrades to the behaviour the player had
// before the corresponding feature existed, so an old-FFmpeg build is
// slower or slightly less precise, never broken.
//
// Each macro is a single point of truth: fix a threshold here rather than
// scattering version arithmetic through the decoders.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/version.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/version.h>
#include <libswscale/version.h>
}

// --- libavcodec ------------------------------------------------------

// AVPacket::opaque, used to tag each packet with the seek generation it
// was read under. FFmpeg 6.0. Without it the demuxer cannot label
// packets, so consumers stop discarding pre-seek packets ahead of the
// codec -- the behaviour before that optimisation, which showed at most
// a frame or two of stale picture immediately after a seek.
#define NAIKAV_HAVE_PACKET_OPAQUE (LIBAVCODEC_VERSION_MAJOR >= 60)

// AVCodecParameters::coded_side_data / nb_coded_side_data, i.e. stream
// level side data reachable from the codec parameters. FFmpeg 7.0.
// Without it, static HDR metadata is recovered by the bitstream probe in
// Demuxer::probeHdrMetadata() instead, which does not need this.
#define NAIKAV_HAVE_CODECPAR_SIDE_DATA (LIBAVCODEC_VERSION_MAJOR >= 61)

// AVCodecParameters::framerate. FFmpeg 7.0. Only ever a fallback -- the
// frame rate normally comes from Demuxer::getVideoFrameRate(), which
// reads the stream and works on every version.
#define NAIKAV_HAVE_CODECPAR_FRAMERATE (LIBAVCODEC_VERSION_MAJOR >= 61)

// --- libavutil -------------------------------------------------------

// Dolby Vision frame side data (AV_FRAME_DATA_DOVI_METADATA and
// AVDOVIMetadata). FFmpeg 5.0. Without it a DV stream is still decoded
// and tone mapped; it just is not labelled as Dolby Vision in the HUD.
#define NAIKAV_HAVE_DOVI_METADATA (LIBAVUTIL_VERSION_MAJOR >= 57)

// av_dovi_find_level() and the AVDOVIDmData level blocks, which is where
// the per-frame L1 luminance lives. FFmpeg 7.0. Without it, Dolby Vision
// gets static tone mapping rather than per-frame.
#define NAIKAV_HAVE_DOVI_LEVELS (LIBAVUTIL_VERSION_MAJOR >= 59)

// HDR10+ dynamic metadata (AV_FRAME_DATA_DYNAMIC_HDR_PLUS). FFmpeg 4.3,
// so present even on the oldest target -- guarded anyway so the intent is
// explicit rather than assumed.
#define NAIKAV_HAVE_HDR10PLUS                 \
    (LIBAVUTIL_VERSION_MAJOR > 56 ||          \
     (LIBAVUTIL_VERSION_MAJOR == 56 && LIBAVUTIL_VERSION_MINOR >= 31))

// --- libswscale ------------------------------------------------------

// The rewritten swscale API: a public SwsContext (so `threads` can be set
// directly), sws_scale_frame() and sws_free_context(). FFmpeg 7.1.
//
// This one costs real performance when absent. Only the new API slices a
// conversion across cores; the legacy path is single-threaded, which on a
// 4K frame measured 95 ms against roughly 27 ms threaded. It also takes
// colour properties from the context via sws_setColorspaceDetails()
// rather than from the frames.
//
// The two are not bit-identical, but the difference is scaler rounding
// rather than anything visible: compared over a tone-mapped 640x360
// frame, 93.4% of bytes matched exactly, the mean absolute difference was
// 0.07 of 255, and the largest difference anywhere was 11 -- on 8 bytes
// out of 691200. A wrong colour matrix would show systematic shifts of
// tens of code values across whole regions instead.
//
// Define NAIKAV_FORCE_LEGACY_SWS to take the legacy path on a machine
// whose FFmpeg is new enough for either. That is the only way to exercise
// the ARM64/Raspberry Pi code path on a development machine, so it is
// kept rather than being a throwaway.
#if defined(NAIKAV_FORCE_LEGACY_SWS)
  #define NAIKAV_HAVE_SWS_THREADED 0
#else
  #define NAIKAV_HAVE_SWS_THREADED (LIBSWSCALE_VERSION_MAJOR >= 8)
#endif

// --- channel layouts -------------------------------------------------

// The AVChannelLayout struct and its av_channel_layout_* API, which
// replaced the old uint64_t bitmask. FFmpeg 5.1 (libavutil 57.24).
#define NAIKAV_HAVE_CH_LAYOUT                 \
    (LIBAVUTIL_VERSION_MAJOR > 57 ||          \
     (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR >= 24))

// av_find_best_stream()'s decoder-out parameter became `const AVCodec **`
// in FFmpeg 5.0; before that it was `AVCodec **`.
#define NAIKAV_HAVE_CONST_CODEC_PTR (LIBAVFORMAT_VERSION_MAJOR >= 59)

// The type to declare for that out parameter, so the call compiles either
// way without the call site caring.
#if NAIKAV_HAVE_CONST_CODEC_PTR
using NaikavBestStreamCodec = const AVCodec*;
#else
using NaikavBestStreamCodec = AVCodec*;
#endif

// ---------------------------------------------------------------------
// Channel-layout helpers
// ---------------------------------------------------------------------
//
// Only the two operations this player actually performs are wrapped --
// "give a frame the default layout for N channels" and "name that layout"
// -- rather than the whole API, so the old-FFmpeg branch stays small
// enough to be obviously correct.

// Give `frame` the default channel layout for `channels` channels.
inline void naikavSetFrameChannelLayout(AVFrame* frame, int channels) {
    if (!frame) {
        return;
    }
#if NAIKAV_HAVE_CH_LAYOUT
    av_channel_layout_default(&frame->ch_layout, channels);
#else
    frame->channel_layout =
        static_cast<uint64_t>(av_get_default_channel_layout(channels));
    frame->channels = channels;
#endif
}

// Write the name of the default layout for `channels` channels into `buf`
// ("stereo", "5.1", ...), as libavfilter's abuffer argument expects.
inline void naikavDescribeDefaultChannelLayout(int channels, char* buf,
                                               size_t size) {
    if (!buf || size == 0) {
        return;
    }
    buf[0] = '\0';
#if NAIKAV_HAVE_CH_LAYOUT
    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    av_channel_layout_describe(&layout, buf, size);
    av_channel_layout_uninit(&layout);
#else
    const uint64_t mask =
        static_cast<uint64_t>(av_get_default_channel_layout(channels));
    av_get_channel_layout_string(buf, static_cast<int>(size), channels, mask);
#endif
}

// ---------------------------------------------------------------------
// Seek-generation tagging helpers
// ---------------------------------------------------------------------

#include <cstdint>

// Tag a packet with the seek generation it was read under. A no-op where
// AVPacket has nowhere to carry it.
inline void naikavTagPacketGeneration(AVPacket* packet, uint64_t generation) {
#if NAIKAV_HAVE_PACKET_OPAQUE
    if (packet) {
        packet->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
    }
#else
    (void)packet;
    (void)generation;
#endif
}

// Read back the generation a packet was tagged with.
//
// `currentGeneration` is returned verbatim when this FFmpeg cannot carry
// the tag, so the caller's "is this packet stale?" comparison always
// says no and every packet is decoded -- which is exactly the behaviour
// that predates the tagging.
inline uint64_t naikavPacketGeneration(const AVPacket* packet,
                                       uint64_t currentGeneration) {
#if NAIKAV_HAVE_PACKET_OPAQUE
    (void)currentGeneration;
    return packet ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(packet->opaque))
                  : 0;
#else
    (void)packet;
    return currentGeneration;
#endif
}
