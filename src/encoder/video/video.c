#include "../../../include/encoder/video/video.h"
#include "../../../include/utils.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

bool gsr_video_encoder_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame, size_t replay_buffer_num_packets) {
    assert(!encoder->started);
    encoder->num_recording_destinations = 0;
    encoder->recording_destination_id = 0;

    if(pthread_mutex_init(&encoder->file_write_mutex, NULL) != 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_start: failed to create mutex\n");
        return false;
    }

    memset(&encoder->replay_buffer, 0, sizeof(encoder->replay_buffer));
    if(replay_buffer_num_packets > 0) {
        if(!gsr_replay_buffer_init(&encoder->replay_buffer, replay_buffer_num_packets)) {
            fprintf(stderr, "gsr error: gsr_video_encoder_start: failed to create replay buffer\n");
            goto error;
        }
        encoder->has_replay_buffer = true;
    }

    bool res = encoder->start(encoder, video_codec_context, frame);
    if(res) {
        encoder->started = true;
        return true;
    } else {
        goto error;
    }

    error:
    pthread_mutex_destroy(&encoder->file_write_mutex);
    gsr_replay_buffer_deinit(&encoder->replay_buffer);
    return false;
}

void gsr_video_encoder_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context) {
    assert(encoder->started);
    encoder->started = false;
    pthread_mutex_destroy(&encoder->file_write_mutex);
    gsr_replay_buffer_deinit(&encoder->replay_buffer);
    encoder->has_replay_buffer = false;
    encoder->num_recording_destinations = 0;
    encoder->recording_destination_id = 0;
    encoder->destroy(encoder, video_codec_context);
}

void gsr_video_encoder_copy_textures_to_frame(gsr_video_encoder *encoder, AVFrame *frame, gsr_color_conversion *color_conversion) {
    assert(encoder->started);
    if(encoder->copy_textures_to_frame)
        encoder->copy_textures_to_frame(encoder, frame, color_conversion);
}

void gsr_video_encoder_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color) {
    assert(encoder->started);
    encoder->get_textures(encoder, textures, num_textures, destination_color);
}

void gsr_video_encoder_receive_packets(gsr_video_encoder *encoder, AVCodecContext *codec_context, int64_t pts, int stream_index) {
    for (;;) {
        AVPacket *av_packet = av_packet_alloc();
        if(!av_packet)
            break;

        av_packet->data = NULL;
        av_packet->size = 0;
        int res = avcodec_receive_packet(codec_context, av_packet);
        if(res == 0) { // we have a packet, send the packet to the muxer
            av_packet->stream_index = stream_index;
            av_packet->pts = pts;
            av_packet->dts = pts;

            if(encoder->has_replay_buffer) {
                const double time_now = clock_get_monotonic_seconds();
                if(!gsr_replay_buffer_append(&encoder->replay_buffer, av_packet, time_now))
                    fprintf(stderr, "gsr error: gsr_video_encoder_receive_packets: failed to add replay buffer data\n");
            }

            pthread_mutex_lock(&encoder->file_write_mutex);
            const bool is_keyframe = av_packet->flags & AV_PKT_FLAG_KEY;
            for(size_t i = 0; i < encoder->num_recording_destinations; ++i) {
                gsr_video_encoder_recording_destination *recording_destination = &encoder->recording_destinations[i];
                if(recording_destination->codec_context != codec_context)
                    continue;

                if(is_keyframe)
                    recording_destination->has_received_keyframe = true;
                else if(!recording_destination->has_received_keyframe)
                    continue;

                av_packet->pts = pts - recording_destination->start_pts;
                av_packet->dts = pts - recording_destination->start_pts;

                av_packet_rescale_ts(av_packet, codec_context->time_base, recording_destination->stream->time_base);
                // TODO: Is av_interleaved_write_frame needed?. Answer: might be needed for mkv but dont use it! it causes frames to be inconsistent, skipping frames and duplicating frames.
                // TODO: av_interleaved_write_frame might be needed for cfr, or always for flv
                const int ret = av_write_frame(recording_destination->format_context, av_packet);
                if(ret < 0) {
                    char error_buffer[AV_ERROR_MAX_STRING_SIZE];
                    if(av_strerror(ret, error_buffer, sizeof(error_buffer)) < 0)
                        snprintf(error_buffer, sizeof(error_buffer), "Unknown error");
                    fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", av_packet->stream_index, error_buffer, ret);
                }
            }
            pthread_mutex_unlock(&encoder->file_write_mutex);

            av_packet_free(&av_packet);
        } else if (res == AVERROR(EAGAIN)) { // we have no packet
                                             // fprintf(stderr, "No packet!\n");
            av_packet_free(&av_packet);
            break;
        } else if (res == AVERROR_EOF) { // this is the end of the stream
            av_packet_free(&av_packet);
            fprintf(stderr, "End of stream!\n");
            break;
        } else {
            av_packet_free(&av_packet);
            fprintf(stderr, "Unexpected error: %d\n", res);
            break;
        }
    }
}

size_t gsr_video_encoder_add_recording_destination(gsr_video_encoder *encoder, AVCodecContext *codec_context, AVFormatContext *format_context, AVStream *stream, int64_t start_pts) {
    if(encoder->num_recording_destinations >= GSR_MAX_RECORDING_DESTINATIONS) {
        fprintf(stderr, "gsr error: gsr_video_encoder_add_recording_destination: failed to add destination, reached the max amount of recording destinations (%d)\n", GSR_MAX_RECORDING_DESTINATIONS);
        return (size_t)-1;
    }

    for(size_t i = 0; i < encoder->num_recording_destinations; ++i) {
        if(encoder->recording_destinations[i].stream == stream) {
            fprintf(stderr, "gsr error: gsr_video_encoder_add_recording_destination: failed to add destination, the stream %p already exists as an output\n", (void*)stream);
            return (size_t)-1;
        }
    }

    pthread_mutex_lock(&encoder->file_write_mutex);
    gsr_video_encoder_recording_destination *recording_destination = &encoder->recording_destinations[encoder->num_recording_destinations];
    recording_destination->id = encoder->recording_destination_id;
    recording_destination->codec_context = codec_context;
    recording_destination->format_context = format_context;
    recording_destination->stream = stream;
    recording_destination->start_pts = start_pts;
    recording_destination->has_received_keyframe = false;

    ++encoder->recording_destination_id;
    ++encoder->num_recording_destinations;
    pthread_mutex_unlock(&encoder->file_write_mutex);

    return recording_destination->id;
}

bool gsr_video_encoder_remove_recording_destination(gsr_video_encoder *encoder, size_t id) {
    bool found = false;
    pthread_mutex_lock(&encoder->file_write_mutex);
    for(size_t i = 0; i < encoder->num_recording_destinations; ++i) {
        if(encoder->recording_destinations[i].id == id) {
            encoder->recording_destinations[i] = encoder->recording_destinations[encoder->num_recording_destinations - 1];
            --encoder->num_recording_destinations;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&encoder->file_write_mutex);
    return found;
}
