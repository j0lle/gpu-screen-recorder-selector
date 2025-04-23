#ifndef GSR_REPLAY_BUFFER_H
#define GSR_REPLAY_BUFFER_H

#include <pthread.h>
#include <stdbool.h>
#include <libavcodec/packet.h>

typedef struct gsr_replay_buffer gsr_replay_buffer;

typedef struct {
    AVPacket packet;
    int ref_counter;
    double timestamp;
} gsr_av_packet;

gsr_av_packet* gsr_av_packet_create(const AVPacket *av_packet, double timestamp);
gsr_av_packet* gsr_av_packet_ref(gsr_av_packet *self);
void gsr_av_packet_unref(gsr_av_packet *self);

struct gsr_replay_buffer {
    gsr_av_packet **packets;
    size_t capacity_num_packets;
    size_t num_packets;
    size_t index;
    pthread_mutex_t mutex;
    bool mutex_initialized;
    gsr_replay_buffer *original_replay_buffer;
};

bool gsr_replay_buffer_init(gsr_replay_buffer *self, size_t replay_buffer_num_packets);
void gsr_replay_buffer_deinit(gsr_replay_buffer *self);

bool gsr_replay_buffer_append(gsr_replay_buffer *self, const AVPacket *av_packet, double timestamp);
void gsr_replay_buffer_clear(gsr_replay_buffer *self);
gsr_av_packet* gsr_replay_buffer_get_packet_at_index(gsr_replay_buffer *self, size_t index);
/* The clone has to be deinitialized before the replay buffer it clones */
bool gsr_replay_buffer_clone(gsr_replay_buffer *self, gsr_replay_buffer *destination);
/* Returns 0 if replay buffer is empty */
size_t gsr_replay_buffer_find_packet_index_by_time_passed(gsr_replay_buffer *self, int seconds);
/* Returns -1 if not found */
size_t gsr_replay_buffer_find_keyframe(gsr_replay_buffer *self, size_t start_index, int stream_index, bool invert_stream_index);

#endif /* GSR_REPLAY_BUFFER_H */