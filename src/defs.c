#include "../include/defs.h"
#include <assert.h>

bool video_codec_is_hdr(gsr_video_codec video_codec) {
    // TODO: Vulkan
    switch(video_codec) {
        case GSR_VIDEO_CODEC_HEVC_HDR:
        case GSR_VIDEO_CODEC_AV1_HDR:
            return true;
        default:
            return false;
    }
}

gsr_video_codec hdr_video_codec_to_sdr_video_codec(gsr_video_codec video_codec) {
    // TODO: Vulkan
    switch(video_codec) {
        case GSR_VIDEO_CODEC_HEVC_HDR:
            return GSR_VIDEO_CODEC_HEVC;
        case GSR_VIDEO_CODEC_AV1_HDR:
            return GSR_VIDEO_CODEC_AV1;
        default:
            return video_codec;
    }
}

gsr_color_depth video_codec_to_bit_depth(gsr_video_codec video_codec) {
    // TODO: 10-bit Vulkan
    switch(video_codec) {
        case GSR_VIDEO_CODEC_HEVC_HDR:
        case GSR_VIDEO_CODEC_HEVC_10BIT:
        case GSR_VIDEO_CODEC_AV1_HDR:
        case GSR_VIDEO_CODEC_AV1_10BIT:
            return GSR_COLOR_DEPTH_10_BITS;
        default:
            return GSR_COLOR_DEPTH_8_BITS;
    }
}

const char* video_codec_to_string(gsr_video_codec video_codec) {
    switch(video_codec) {
        case GSR_VIDEO_CODEC_H264:        return "h264";
        case GSR_VIDEO_CODEC_HEVC:        return "hevc";
        case GSR_VIDEO_CODEC_HEVC_HDR:    return "hevc_hdr";
        case GSR_VIDEO_CODEC_HEVC_10BIT:  return "hevc_10bit";
        case GSR_VIDEO_CODEC_AV1:         return "av1";
        case GSR_VIDEO_CODEC_AV1_HDR:     return "av1_hdr";
        case GSR_VIDEO_CODEC_AV1_10BIT:   return "av1_10bit";
        case GSR_VIDEO_CODEC_VP8:         return "vp8";
        case GSR_VIDEO_CODEC_VP9:         return "vp9";
        case GSR_VIDEO_CODEC_H264_VULKAN: return "h264_vulkan";
        case GSR_VIDEO_CODEC_HEVC_VULKAN: return "hevc_vulkan";
    }
    return "";
}

// bool video_codec_is_hevc(gsr_video_codec video_codec) {
//     // TODO: 10-bit vulkan
//     switch(video_codec) {
//         case GSR_VIDEO_CODEC_HEVC:
//         case GSR_VIDEO_CODEC_HEVC_HDR:
//         case GSR_VIDEO_CODEC_HEVC_10BIT:
//         case GSR_VIDEO_CODEC_HEVC_VULKAN:
//             return true;
//         default:
//             return false;
//     }
// }

bool video_codec_is_av1(gsr_video_codec video_codec) {
    // TODO: Vulkan
    switch(video_codec) {
        case GSR_VIDEO_CODEC_AV1:
        case GSR_VIDEO_CODEC_AV1_HDR:
        case GSR_VIDEO_CODEC_AV1_10BIT:
            return true;
        default:
            return false;
    }
}

bool video_codec_is_vulkan(gsr_video_codec video_codec) {
    switch(video_codec) {
        case GSR_VIDEO_CODEC_H264_VULKAN:
        case GSR_VIDEO_CODEC_HEVC_VULKAN:
            return true;
        default:
            return false;
    }
}

const char* audio_codec_get_name(gsr_audio_codec audio_codec) {
    switch(audio_codec) {
        case GSR_AUDIO_CODEC_AAC:  return "aac";
        case GSR_AUDIO_CODEC_OPUS: return "opus";
        case GSR_AUDIO_CODEC_FLAC: return "flac";
    }
    assert(false);
    return "";
}
