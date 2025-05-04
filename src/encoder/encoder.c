#include "../../include/encoder/encoder.h"
#include "../../include/utils.h"

#include <string.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

bool gsr_encoder_init(gsr_encoder *self, gsr_replay_storage replay_storage, size_t replay_buffer_num_packets, double replay_buffer_time, const char *replay_directory) {
    memset(self, 0, sizeof(*self));
    self->num_recording_destinations = 0;
    self->recording_destination_id_counter = 0;

    if(pthread_mutex_init(&self->file_write_mutex, NULL) != 0) {
        fprintf(stderr, "gsr error: gsr_encoder_init: failed to create mutex\n");
        return false;
    }
    self->mutex_created = true;

    if(replay_buffer_num_packets > 0) {
        self->replay_buffer = gsr_replay_buffer_create(replay_storage, replay_directory, replay_buffer_time, replay_buffer_num_packets);
        if(!self->replay_buffer) {
            fprintf(stderr, "gsr error: gsr_encoder_init: failed to create replay buffer\n");
            gsr_encoder_deinit(self);
            return false;
        }
    }

    return true;
}

void gsr_encoder_deinit(gsr_encoder *self)  {
    if(self->mutex_created) {
        self->mutex_created = false;
        pthread_mutex_destroy(&self->file_write_mutex);
    }

    if(self->replay_buffer) {
        gsr_replay_buffer_destroy(self->replay_buffer);
        self->replay_buffer = NULL;
    }

    self->num_recording_destinations = 0;
    self->recording_destination_id_counter = 0;
}

void gsr_encoder_receive_packets(gsr_encoder *self, AVCodecContext *codec_context, int64_t pts, int stream_index) {
    for(;;) {
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

            if(self->replay_buffer) {
                const double time_now = clock_get_monotonic_seconds();
                if(!gsr_replay_buffer_append(self->replay_buffer, av_packet, time_now))
                    fprintf(stderr, "gsr error: gsr_encoder_receive_packets: failed to add replay buffer data\n");
            }

            pthread_mutex_lock(&self->file_write_mutex);
            const bool is_keyframe = av_packet->flags & AV_PKT_FLAG_KEY;
            for(size_t i = 0; i < self->num_recording_destinations; ++i) {
                gsr_encoder_recording_destination *recording_destination = &self->recording_destinations[i];
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
                    fprintf(stderr, "gsr error: gsr_encoder_receive_packets: failed to write frame index %d to muxer, reason: %s (%d)\n", av_packet->stream_index, error_buffer, ret);
                }
            }
            pthread_mutex_unlock(&self->file_write_mutex);

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

size_t gsr_encoder_add_recording_destination(gsr_encoder *self, AVCodecContext *codec_context, AVFormatContext *format_context, AVStream *stream, int64_t start_pts) {
    if(self->num_recording_destinations >= GSR_MAX_RECORDING_DESTINATIONS) {
        fprintf(stderr, "gsr error: gsr_encoder_add_recording_destination: failed to add destination, reached the max amount of recording destinations (%d)\n", GSR_MAX_RECORDING_DESTINATIONS);
        return (size_t)-1;
    }

    for(size_t i = 0; i < self->num_recording_destinations; ++i) {
        if(self->recording_destinations[i].stream == stream) {
            fprintf(stderr, "gsr error: gsr_encoder_add_recording_destination: failed to add destination, the stream %p already exists as an output\n", (void*)stream);
            return (size_t)-1;
        }
    }

    pthread_mutex_lock(&self->file_write_mutex);
    gsr_encoder_recording_destination *recording_destination = &self->recording_destinations[self->num_recording_destinations];
    recording_destination->id = self->recording_destination_id_counter;
    recording_destination->codec_context = codec_context;
    recording_destination->format_context = format_context;
    recording_destination->stream = stream;
    recording_destination->start_pts = start_pts;
    recording_destination->has_received_keyframe = false;

    ++self->recording_destination_id_counter;
    ++self->num_recording_destinations;
    pthread_mutex_unlock(&self->file_write_mutex);

    return recording_destination->id;
}

bool gsr_encoder_remove_recording_destination(gsr_encoder *self, size_t id) {
    bool found = false;
    pthread_mutex_lock(&self->file_write_mutex);
    for(size_t i = 0; i < self->num_recording_destinations; ++i) {
        if(self->recording_destinations[i].id == id) {
            self->recording_destinations[i] = self->recording_destinations[self->num_recording_destinations - 1];
            --self->num_recording_destinations;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&self->file_write_mutex);
    return found;
}
