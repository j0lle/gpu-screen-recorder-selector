#include "../include/replay_buffer.h"
#include "../include/utils.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libavutil/mem.h>

gsr_av_packet* gsr_av_packet_create(const AVPacket *av_packet, double timestamp) {
    gsr_av_packet *self = malloc(sizeof(gsr_av_packet));
    if(!self)
        return NULL;

    self->ref_counter = 1;
    self->packet = *av_packet;
    // Why are we doing this you ask? there is a new ffmpeg bug that causes cpu usage to increase over time when you have
    // packets that are not being free'd until later. So we copy the packet data, free the packet and then reconstruct
    // the packet later on when we need it, to keep packets alive only for a short period.
    self->packet.data = av_memdup(av_packet->data, av_packet->size);
    self->timestamp = timestamp;
    if(!self->packet.data) {
        free(self);
        return NULL;
    }

    return self;
}

gsr_av_packet* gsr_av_packet_ref(gsr_av_packet *self) {
    if(self->ref_counter >= 1)
        ++self->ref_counter;
    return self;
}

static void gsr_av_packet_free(gsr_av_packet *self) {
    self->ref_counter = 0;
    if(self->packet.data) {
        av_free(self->packet.data);
        self->packet.data = NULL;
    }
    free(self);
}

void gsr_av_packet_unref(gsr_av_packet *self) {
    if(self->ref_counter >= 1)
        --self->ref_counter;

    if(self->ref_counter <= 0)
        gsr_av_packet_free(self);
}

bool gsr_replay_buffer_init(gsr_replay_buffer *self, size_t replay_buffer_num_packets) {
    assert(replay_buffer_num_packets > 0);
    memset(self, 0, sizeof(*self));
    self->mutex_initialized = false;
    self->original_replay_buffer = NULL;
    if(pthread_mutex_init(&self->mutex, NULL) != 0)
        return false;

    self->mutex_initialized = true;
    self->capacity_num_packets = replay_buffer_num_packets;
    self->num_packets = 0;
    self->index = 0;
    self->packets = calloc(self->capacity_num_packets, sizeof(gsr_av_packet*));
    if(!self->packets) {
        gsr_replay_buffer_deinit(self);
        return false;
    }
    return true;
}

static void gsr_replay_buffer_lock(gsr_replay_buffer *self) {
    if(self->original_replay_buffer) {
        gsr_replay_buffer_lock(self->original_replay_buffer);
        return;
    }

    if(self->mutex_initialized)
        pthread_mutex_lock(&self->mutex);
}

static void gsr_replay_buffer_unlock(gsr_replay_buffer *self) {
    if(self->original_replay_buffer) {
        gsr_replay_buffer_unlock(self->original_replay_buffer);
        return;
    }

    if(self->mutex_initialized)
        pthread_mutex_unlock(&self->mutex);
}

void gsr_replay_buffer_deinit(gsr_replay_buffer *self) {
    gsr_replay_buffer_lock(self);
    for(size_t i = 0; i < self->num_packets; ++i) {
        if(self->packets[i]) {
            gsr_av_packet_unref(self->packets[i]);
            self->packets[i] = NULL;
        }
    }
    self->num_packets = 0;
    gsr_replay_buffer_unlock(self);

    if(self->packets) {
        free(self->packets);
        self->packets = NULL;
    }

    self->capacity_num_packets = 0;
    self->index = 0;

    if(self->mutex_initialized && !self->original_replay_buffer) {
        pthread_mutex_destroy(&self->mutex);
        self->mutex_initialized = false;
    }

    self->original_replay_buffer = NULL;
}

bool gsr_replay_buffer_append(gsr_replay_buffer *self, const AVPacket *av_packet, double timestamp) {
    gsr_replay_buffer_lock(self);
    gsr_av_packet *packet = gsr_av_packet_create(av_packet, timestamp);
    if(!packet) {
        gsr_replay_buffer_unlock(self);
        return false;
    }

    if(self->packets[self->index]) {
        gsr_av_packet_unref(self->packets[self->index]);
        self->packets[self->index] = NULL;
    }
    self->packets[self->index] = packet;

    self->index = (self->index + 1) % self->capacity_num_packets;
    ++self->num_packets;
    if(self->num_packets > self->capacity_num_packets)
        self->num_packets = self->capacity_num_packets;

    gsr_replay_buffer_unlock(self);
    return true;
}

void gsr_replay_buffer_clear(gsr_replay_buffer *self) {
    gsr_replay_buffer_lock(self);
    for(size_t i = 0; i < self->num_packets; ++i) {
        if(self->packets[i]) {
            gsr_av_packet_unref(self->packets[i]);
            self->packets[i] = NULL;
        }
    }
    self->num_packets = 0;
    self->index = 0;
    gsr_replay_buffer_unlock(self);
}

gsr_av_packet* gsr_replay_buffer_get_packet_at_index(gsr_replay_buffer *self, size_t index) {
    assert(index < self->num_packets);
    size_t start_index = 0;
    if(self->num_packets < self->capacity_num_packets)
        start_index = self->num_packets - self->index;
    else
        start_index = self->index;

    const size_t offset = (start_index + index) % self->capacity_num_packets;
    return self->packets[offset];
}

bool gsr_replay_buffer_clone(gsr_replay_buffer *self, gsr_replay_buffer *destination) {
    gsr_replay_buffer_lock(self);
    memset(destination, 0, sizeof(*destination));
    destination->original_replay_buffer = self;
    destination->mutex = self->mutex;
    destination->capacity_num_packets = self->capacity_num_packets;
    destination->mutex_initialized = self->mutex_initialized;
    destination->index = self->index;
    destination->packets = calloc(destination->capacity_num_packets, sizeof(gsr_av_packet*));
    if(!destination->packets) {
        gsr_replay_buffer_unlock(self);
        return false;
    }

    destination->num_packets = self->num_packets;
    for(size_t i = 0; i < destination->num_packets; ++i) {
        destination->packets[i] = gsr_av_packet_ref(self->packets[i]);
    }

    gsr_replay_buffer_unlock(self);
    return true;
}

/* Binary search */
size_t gsr_replay_buffer_find_packet_index_by_time_passed(gsr_replay_buffer *self, int seconds) {
    gsr_replay_buffer_lock(self);

    const double now = clock_get_monotonic_seconds();
    if(self->num_packets == 0) {
        gsr_replay_buffer_unlock(self);
        return 0;
    }

    size_t lower_bound = 0;
    size_t upper_bound = self->num_packets;
    size_t index = 0;

    for(;;) {
        index = lower_bound + (upper_bound - lower_bound) / 2;
        const gsr_av_packet *packet = gsr_replay_buffer_get_packet_at_index(self, index);
        const double time_passed_since_packet = now - packet->timestamp;
        if(time_passed_since_packet >= seconds) {
            if(lower_bound == index)
                break;
            lower_bound = index;
        } else {
            if(upper_bound == index)
                break;
            upper_bound = index;
        }
    }

    gsr_replay_buffer_unlock(self);
    return index;
}

size_t gsr_replay_buffer_find_keyframe(gsr_replay_buffer *self, size_t start_index, int stream_index, bool invert_stream_index) {
    assert(start_index < self->num_packets);
    size_t keyframe_index = (size_t)-1;
    gsr_replay_buffer_lock(self);
    for(size_t i = start_index; i < self->num_packets; ++i) {
        const gsr_av_packet *packet = gsr_replay_buffer_get_packet_at_index(self, i);
        if((packet->packet.flags & AV_PKT_FLAG_KEY) && (invert_stream_index ? packet->packet.stream_index != stream_index : packet->packet.stream_index == stream_index)) {
            keyframe_index = i;
            break;
        }
    }
    gsr_replay_buffer_unlock(self);
    return keyframe_index;
}
