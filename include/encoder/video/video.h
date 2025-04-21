#ifndef GSR_ENCODER_VIDEO_H
#define GSR_ENCODER_VIDEO_H

#include "../../color_conversion.h"
#include "../../replay_buffer.h"
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#define GSR_MAX_RECORDING_DESTINATIONS 128

typedef struct gsr_video_encoder gsr_video_encoder;
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFormatContext AVFormatContext;
typedef struct AVFrame AVFrame;
typedef struct AVStream AVStream;

typedef struct {
    size_t id;
    AVCodecContext *codec_context;
    AVFormatContext *format_context;
    AVStream *stream;
    int64_t start_pts;
    bool has_received_keyframe;
} gsr_video_encoder_recording_destination;

struct gsr_video_encoder {
    bool (*start)(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame);
    void (*destroy)(gsr_video_encoder *encoder, AVCodecContext *video_codec_context);
    void (*copy_textures_to_frame)(gsr_video_encoder *encoder, AVFrame *frame, gsr_color_conversion *color_conversion); /* Can be NULL */
    /* |textures| should be able to fit 2 elements */
    void (*get_textures)(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color);

    void *priv;
    bool started;
    gsr_replay_buffer replay_buffer;
    bool has_replay_buffer;
    pthread_mutex_t file_write_mutex;

    gsr_video_encoder_recording_destination recording_destinations[GSR_MAX_RECORDING_DESTINATIONS];
    size_t num_recording_destinations;
    size_t recording_destination_id;
};

/* Set |replay_buffer_time_seconds| and |fps| to 0 to disable replay buffer */
bool gsr_video_encoder_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame, size_t replay_buffer_num_packets);
void gsr_video_encoder_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context);
void gsr_video_encoder_copy_textures_to_frame(gsr_video_encoder *encoder, AVFrame *frame, gsr_color_conversion *color_conversion);
void gsr_video_encoder_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color);

void gsr_video_encoder_receive_packets(gsr_video_encoder *encoder, AVCodecContext *codec_context, int64_t pts, int stream_index);
/* Returns the id to the recording destination, or -1 on error */
size_t gsr_video_encoder_add_recording_destination(gsr_video_encoder *encoder, AVCodecContext *codec_context, AVFormatContext *format_context, AVStream *stream, int64_t start_pts);
bool gsr_video_encoder_remove_recording_destination(gsr_video_encoder *encoder, size_t id);

#endif /* GSR_ENCODER_VIDEO_H */
