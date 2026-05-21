#include "../../include/capture/wayland_window.h"
#include "../../include/color_conversion.h"
#include "../../include/egl.h"
#include "../../include/utils.h"
#include "../../include/window/window.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

typedef struct {
    struct ext_foreign_toplevel_handle_v1 *handle;
    char *title;
    char *app_id;
    char *identifier;
    bool done;
    bool closed;
} gsr_wayland_toplevel;

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct ext_foreign_toplevel_list_v1 *toplevel_list;
    struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_source_manager;
    struct ext_image_copy_capture_manager_v1 *copy_manager;

    gsr_wayland_toplevel **toplevels;
    size_t num_toplevels;
    size_t toplevels_capacity;
} gsr_wayland_window_context;

typedef struct {
    gsr_capture_wayland_window_params params;

    gsr_wayland_window_context wl;
    gsr_wayland_toplevel *selected_toplevel;

    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;

    struct wl_buffer *buffer;
    void *buffer_data;
    uint8_t *rgba_data;
    size_t buffer_size_bytes;
    uint32_t shm_format;
    int buffer_stride;
    bool supports_argb8888;
    bool supports_xrgb8888;
    bool supports_bgr888;
    bool supports_rgb888;
    bool constraints_done;
    bool session_stopped;
    bool frame_ready;
    bool frame_in_flight;
    bool has_frame;
    bool damaged;
    bool should_stop;
    bool stop_is_error;
    bool do_capture;
    bool clear_next_frame;

    vec2i capture_size;
    uint32_t frame_transform;
    unsigned int texture_id;
} gsr_capture_wayland_window;

static int gsr_memfd_create(const char *name) {
#ifdef SYS_memfd_create
    return syscall(SYS_memfd_create, name, MFD_CLOEXEC);
#else
    (void)name;
    errno = ENOSYS;
    return -1;
#endif
}

static bool array_ensure_capacity(void **array, size_t size, size_t *capacity_items, size_t element_size) {
    if(size < *capacity_items)
        return true;

    const size_t new_capacity_items = *capacity_items == 0 ? 8 : (*capacity_items * 2);
    void *new_data = realloc(*array, new_capacity_items * element_size);
    if(!new_data)
        return false;

    *array = new_data;
    *capacity_items = new_capacity_items;
    return true;
}

static char* string_replace(char *dst, const char *src) {
    free(dst);
    return src ? strdup(src) : NULL;
}

static int ascii_tolower(int c) {
    if(c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static bool string_case_contains(const char *str, const char *substr) {
    if(!str || !substr)
        return false;

    const size_t substr_len = strlen(substr);
    if(substr_len == 0)
        return true;

    for(const char *p = str; *p; ++p) {
        size_t i = 0;
        while(i < substr_len && p[i] && ascii_tolower(p[i]) == ascii_tolower(substr[i]))
            ++i;

        if(i == substr_len)
            return true;
    }

    return false;
}

static void print_sanitized_stderr(const char *str) {
    if(!str) {
        fputs("(null)", stderr);
        return;
    }

    for(const char *p = str; *p; ++p) {
        const unsigned char c = *(const unsigned char*)p;
        if(c < 32 || c == 127 || c == '|')
            fputc('_', stderr);
        else
            fputc(c, stderr);
    }
}

static void toplevel_handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    gsr_wayland_toplevel *toplevel = data;
    toplevel->closed = true;
}

static void toplevel_handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    gsr_wayland_toplevel *toplevel = data;
    toplevel->done = true;
}

static void toplevel_handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *title) {
    (void)handle;
    gsr_wayland_toplevel *toplevel = data;
    toplevel->title = string_replace(toplevel->title, title);
}

static void toplevel_handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *app_id) {
    (void)handle;
    gsr_wayland_toplevel *toplevel = data;
    toplevel->app_id = string_replace(toplevel->app_id, app_id);
}

static void toplevel_handle_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *identifier) {
    (void)handle;
    gsr_wayland_toplevel *toplevel = data;
    toplevel->identifier = string_replace(toplevel->identifier, identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
    .closed = toplevel_handle_closed,
    .done = toplevel_handle_done,
    .title = toplevel_handle_title,
    .app_id = toplevel_handle_app_id,
    .identifier = toplevel_handle_identifier,
};

static void toplevel_list_handle_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list, struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)list;
    gsr_wayland_window_context *ctx = data;
    if(!array_ensure_capacity((void**)&ctx->toplevels, ctx->num_toplevels, &ctx->toplevels_capacity, sizeof(gsr_wayland_toplevel*))) {
        fprintf(stderr, "gsr error: wayland window capture: failed to allocate toplevel list\n");
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }

    gsr_wayland_toplevel *toplevel = calloc(1, sizeof(gsr_wayland_toplevel));
    if(!toplevel) {
        fprintf(stderr, "gsr error: wayland window capture: failed to allocate toplevel\n");
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }

    *toplevel = (gsr_wayland_toplevel) {
        .handle = handle,
        .title = NULL,
        .app_id = NULL,
        .identifier = NULL,
        .done = false,
        .closed = false
    };
    ctx->toplevels[ctx->num_toplevels++] = toplevel;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_handle_listener, toplevel);
}

static void toplevel_list_handle_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
    (void)data;
    (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener toplevel_list_listener = {
    .toplevel = toplevel_list_handle_toplevel,
    .finished = toplevel_list_handle_finished,
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    gsr_wayland_window_context *ctx = data;

    if(strcmp(interface, wl_shm_interface.name) == 0) {
        if(!ctx->shm)
            ctx->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if(strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        if(!ctx->toplevel_list) {
            ctx->toplevel_list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, version < 1 ? version : 1);
            ext_foreign_toplevel_list_v1_add_listener(ctx->toplevel_list, &toplevel_list_listener, ctx);
        }
    } else if(strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
        if(!ctx->toplevel_source_manager)
            ctx->toplevel_source_manager = wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, version < 1 ? version : 1);
    } else if(strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        if(!ctx->copy_manager)
            ctx->copy_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, version < 1 ? version : 1);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static bool wayland_window_context_init(gsr_wayland_window_context *ctx, gsr_window *window) {
    memset(ctx, 0, sizeof(*ctx));

    if(!window || gsr_window_get_display_server(window) != GSR_DISPLAY_SERVER_WAYLAND)
        return false;

    ctx->display = gsr_window_get_display(window);
    if(!ctx->display)
        return false;

    ctx->registry = wl_display_get_registry(ctx->display);
    if(!ctx->registry)
        return false;

    wl_registry_add_listener(ctx->registry, &registry_listener, ctx);
    wl_display_roundtrip(ctx->display);

    wl_display_roundtrip(ctx->display);
    wl_display_roundtrip(ctx->display);
    return true;
}

static void wayland_window_context_deinit(gsr_wayland_window_context *ctx) {
    for(size_t i = 0; i < ctx->num_toplevels; ++i) {
        gsr_wayland_toplevel *toplevel = ctx->toplevels[i];
        if(!toplevel)
            continue;

        if(toplevel->handle) {
            ext_foreign_toplevel_handle_v1_destroy(toplevel->handle);
            toplevel->handle = NULL;
        }
        free(toplevel->title);
        free(toplevel->app_id);
        free(toplevel->identifier);
        free(toplevel);
    }
    free(ctx->toplevels);
    ctx->toplevels = NULL;
    ctx->num_toplevels = 0;
    ctx->toplevels_capacity = 0;

    if(ctx->toplevel_list) {
        ext_foreign_toplevel_list_v1_destroy(ctx->toplevel_list);
        ctx->toplevel_list = NULL;
    }

    if(ctx->toplevel_source_manager) {
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(ctx->toplevel_source_manager);
        ctx->toplevel_source_manager = NULL;
    }

    if(ctx->copy_manager) {
        ext_image_copy_capture_manager_v1_destroy(ctx->copy_manager);
        ctx->copy_manager = NULL;
    }

    if(ctx->shm) {
        wl_shm_destroy(ctx->shm);
        ctx->shm = NULL;
    }

    if(ctx->registry) {
        wl_registry_destroy(ctx->registry);
        ctx->registry = NULL;
    }

    ctx->display = NULL;
}

static bool wayland_window_context_has_capture_support(const gsr_wayland_window_context *ctx) {
    return ctx->toplevel_list && ctx->toplevel_source_manager && ctx->copy_manager && ctx->shm;
}

bool gsr_capture_wayland_window_supported(gsr_window *window) {
    gsr_wayland_window_context ctx;
    if(!wayland_window_context_init(&ctx, window))
        return false;

    const bool supported = wayland_window_context_has_capture_support(&ctx);
    wayland_window_context_deinit(&ctx);
    return supported;
}

bool gsr_capture_wayland_window_list(gsr_window *window, gsr_wayland_window_list_callback callback, void *userdata) {
    if(!callback)
        return false;

    gsr_wayland_window_context ctx;
    if(!wayland_window_context_init(&ctx, window))
        return false;

    const bool supported = wayland_window_context_has_capture_support(&ctx);
    if(supported) {
        for(size_t i = 0; i < ctx.num_toplevels; ++i) {
            const gsr_wayland_toplevel *toplevel = ctx.toplevels[i];
            if(!toplevel)
                continue;

            if(toplevel->closed || !toplevel->done)
                continue;

            const gsr_wayland_window_info info = {
                .identifier = toplevel->identifier,
                .app_id = toplevel->app_id,
                .title = toplevel->title
            };
            callback(&info, userdata);
        }
    }

    wayland_window_context_deinit(&ctx);
    return supported;
}

static bool toplevel_matches(const gsr_wayland_toplevel *toplevel, gsr_wayland_window_match_type match_type, const char *match_value) {
    if(toplevel->closed || !toplevel->done || !match_value)
        return false;

    switch(match_type) {
        case GSR_WAYLAND_WINDOW_MATCH_TYPE_IDENTIFIER:
            return toplevel->identifier && strcmp(toplevel->identifier, match_value) == 0;
        case GSR_WAYLAND_WINDOW_MATCH_TYPE_APP_ID:
            return toplevel->app_id && strcmp(toplevel->app_id, match_value) == 0;
        case GSR_WAYLAND_WINDOW_MATCH_TYPE_TITLE:
            return toplevel->title && string_case_contains(toplevel->title, match_value);
        case GSR_WAYLAND_WINDOW_MATCH_TYPE_NONE:
            return false;
    }

    return false;
}

static int find_matching_toplevel(gsr_wayland_window_context *ctx, gsr_wayland_window_match_type match_type, const char *match_value, gsr_wayland_toplevel **match) {
    int num_matches = 0;
    *match = NULL;

    for(size_t i = 0; i < ctx->num_toplevels; ++i) {
        gsr_wayland_toplevel *toplevel = ctx->toplevels[i];
        if(!toplevel)
            continue;

        if(!toplevel_matches(toplevel, match_type, match_value))
            continue;

        ++num_matches;
        *match = toplevel;
    }

    return num_matches;
}

static void print_toplevel_candidates(gsr_wayland_window_context *ctx) {
    fprintf(stderr, "gsr info: available Wayland windows:\n");
    for(size_t i = 0; i < ctx->num_toplevels; ++i) {
        const gsr_wayland_toplevel *toplevel = ctx->toplevels[i];
        if(!toplevel)
            continue;

        if(toplevel->closed || !toplevel->done)
            continue;

        fprintf(stderr, "  window:id=");
        print_sanitized_stderr(toplevel->identifier);
        fprintf(stderr, " app_id=");
        print_sanitized_stderr(toplevel->app_id);
        fprintf(stderr, " title=");
        print_sanitized_stderr(toplevel->title);
        fputc('\n', stderr);
    }
}

static bool wait_for_matching_toplevel(gsr_wayland_window_context *ctx, gsr_wayland_window_match_type match_type, const char *match_value, double timeout_sec, gsr_wayland_toplevel **match) {
    const double start = clock_get_monotonic_seconds();
    int num_matches = 0;

    for(;;) {
        wl_display_roundtrip(ctx->display);
        num_matches = find_matching_toplevel(ctx, match_type, match_value, match);
        if(num_matches == 1)
            return true;

        if(clock_get_monotonic_seconds() - start >= timeout_sec)
            break;

        usleep(50 * 1000);
    }

    if(num_matches == 0) {
        fprintf(stderr, "gsr error: no Wayland window matched ");
    } else {
        fprintf(stderr, "gsr error: %d Wayland windows matched ", num_matches);
    }

    switch(match_type) {
        case GSR_WAYLAND_WINDOW_MATCH_TYPE_IDENTIFIER:
            fprintf(stderr, "id=");
            break;
        case GSR_WAYLAND_WINDOW_MATCH_TYPE_APP_ID:
            fprintf(stderr, "app_id=");
            break;
        case GSR_WAYLAND_WINDOW_MATCH_TYPE_TITLE:
            fprintf(stderr, "title=");
            break;
        case GSR_WAYLAND_WINDOW_MATCH_TYPE_NONE:
            fprintf(stderr, "selector=");
            break;
    }
    print_sanitized_stderr(match_value);
    fprintf(stderr, "\n");
    print_toplevel_candidates(ctx);
    return false;
}

static void capture_destroy_buffer(gsr_capture_wayland_window *self) {
    if(self->buffer) {
        wl_buffer_destroy(self->buffer);
        self->buffer = NULL;
    }

    if(self->buffer_data) {
        munmap(self->buffer_data, self->buffer_size_bytes);
        self->buffer_data = NULL;
    }

    free(self->rgba_data);
    self->rgba_data = NULL;

    self->buffer_size_bytes = 0;
    self->buffer_stride = 0;
}

static bool capture_create_buffer(gsr_capture_wayland_window *self) {
    capture_destroy_buffer(self);

    if(self->capture_size.x <= 0 || self->capture_size.y <= 0)
        return false;

    if(self->supports_argb8888)
        self->shm_format = WL_SHM_FORMAT_ARGB8888;
    else if(self->supports_xrgb8888)
        self->shm_format = WL_SHM_FORMAT_XRGB8888;
    else if(self->supports_bgr888)
        self->shm_format = WL_SHM_FORMAT_BGR888;
    else
        self->shm_format = WL_SHM_FORMAT_RGB888;

    const int bytes_per_pixel = (self->shm_format == WL_SHM_FORMAT_BGR888 || self->shm_format == WL_SHM_FORMAT_RGB888) ? 3 : 4;
    const int stride = self->capture_size.x * bytes_per_pixel;
    const size_t size = (size_t)stride * (size_t)self->capture_size.y;
    const size_t rgba_size = (size_t)self->capture_size.x * (size_t)self->capture_size.y * 4;
    if(size > INT_MAX) {
        fprintf(stderr, "gsr error: wayland window capture: capture buffer is too large\n");
        return false;
    }

    int fd = gsr_memfd_create("gsr-wayland-window-capture");
    if(fd == -1) {
        fprintf(stderr, "gsr error: wayland window capture: memfd_create failed: %s\n", strerror(errno));
        return false;
    }

    if(ftruncate(fd, size) != 0) {
        fprintf(stderr, "gsr error: wayland window capture: ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(data == MAP_FAILED) {
        fprintf(stderr, "gsr error: wayland window capture: mmap failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(self->wl.shm, fd, (int32_t)size);
    close(fd);
    if(!pool) {
        fprintf(stderr, "gsr error: wayland window capture: wl_shm_create_pool failed\n");
        munmap(data, size);
        return false;
    }

    self->buffer = wl_shm_pool_create_buffer(pool, 0, self->capture_size.x, self->capture_size.y, stride, self->shm_format);
    wl_shm_pool_destroy(pool);
    if(!self->buffer) {
        fprintf(stderr, "gsr error: wayland window capture: wl_shm_pool_create_buffer failed\n");
        munmap(data, size);
        return false;
    }

    self->rgba_data = malloc(rgba_size);
    if(!self->rgba_data) {
        fprintf(stderr, "gsr error: wayland window capture: failed to allocate upload buffer\n");
        wl_buffer_destroy(self->buffer);
        self->buffer = NULL;
        munmap(data, size);
        return false;
    }

    self->buffer_data = data;
    self->buffer_size_bytes = size;
    self->buffer_stride = stride;
    return true;
}

static void capture_destroy_frame(gsr_capture_wayland_window *self) {
    if(self->frame) {
        ext_image_copy_capture_frame_v1_destroy(self->frame);
        self->frame = NULL;
    }
    self->frame_in_flight = false;
}

static gsr_rotation wl_transform_to_gsr_rotation(uint32_t transform) {
    switch(transform) {
        case WL_OUTPUT_TRANSFORM_90: return GSR_ROT_90;
        case WL_OUTPUT_TRANSFORM_180: return GSR_ROT_180;
        case WL_OUTPUT_TRANSFORM_270: return GSR_ROT_270;
        default: return GSR_ROT_0;
    }
}

static void frame_handle_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform) {
    (void)frame;
    gsr_capture_wayland_window *self = data;
    self->frame_transform = transform;
}

static void frame_handle_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void frame_handle_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    (void)data;
    (void)frame;
    (void)tv_sec_hi;
    (void)tv_sec_lo;
    (void)tv_nsec;
}

static void frame_handle_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
    gsr_capture_wayland_window *self = data;
    if(self->frame == frame) {
        self->frame = NULL;
        self->frame_in_flight = false;
    }
    ext_image_copy_capture_frame_v1_destroy(frame);

    self->frame_ready = true;
    self->damaged = true;
}

static void frame_handle_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason) {
    gsr_capture_wayland_window *self = data;
    if(self->frame == frame) {
        self->frame = NULL;
        self->frame_in_flight = false;
    }
    ext_image_copy_capture_frame_v1_destroy(frame);

    if(reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
        self->constraints_done = false;
        self->clear_next_frame = true;
        return;
    }

    fprintf(stderr, "gsr error: wayland window capture frame failed, reason: %u\n", reason);
    self->stop_is_error = reason != EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED;
    self->should_stop = true;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_handle_transform,
    .damage = frame_handle_damage,
    .presentation_time = frame_handle_presentation_time,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
};

static bool capture_queue_frame(gsr_capture_wayland_window *self) {
    if(self->frame || self->session_stopped || self->should_stop || !self->buffer)
        return false;

    self->frame = ext_image_copy_capture_session_v1_create_frame(self->session);
    if(!self->frame)
        return false;

    self->frame_in_flight = true;
    ext_image_copy_capture_frame_v1_add_listener(self->frame, &frame_listener, self);
    ext_image_copy_capture_frame_v1_attach_buffer(self->frame, self->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(self->frame, 0, 0, self->capture_size.x, self->capture_size.y);
    ext_image_copy_capture_frame_v1_capture(self->frame);
    wl_display_flush(self->wl.display);
    return true;
}

static void capture_upload_ready_frame(gsr_capture_wayland_window *self) {
    if(!self->frame_ready)
        return;

    uint8_t *dst = self->rgba_data;

    if(self->shm_format == WL_SHM_FORMAT_BGR888 || self->shm_format == WL_SHM_FORMAT_RGB888) {
        const uint8_t *src = self->buffer_data;
        for(int y = 0; y < self->capture_size.y; ++y) {
            const uint8_t *src_row = src + (size_t)y * self->buffer_stride;
            uint8_t *dst_row = dst + (size_t)y * (size_t)self->capture_size.x * 4;
            for(int x = 0; x < self->capture_size.x; ++x) {
                const uint8_t *pixel = src_row + (size_t)x * 3;
                uint8_t *out = dst_row + (size_t)x * 4;
                if(self->shm_format == WL_SHM_FORMAT_BGR888) {
                    out[0] = pixel[2];
                    out[1] = pixel[1];
                    out[2] = pixel[0];
                } else {
                    out[0] = pixel[0];
                    out[1] = pixel[1];
                    out[2] = pixel[2];
                }
                out[3] = 0xFF;
            }
        }
    } else {
        const uint32_t *src = self->buffer_data;
        const size_t num_pixels = (size_t)self->capture_size.x * (size_t)self->capture_size.y;
        for(size_t i = 0; i < num_pixels; ++i) {
            const uint32_t pixel = src[i];
            dst[i * 4 + 0] = (pixel >> 16) & 0xFF;
            dst[i * 4 + 1] = (pixel >> 8) & 0xFF;
            dst[i * 4 + 2] = pixel & 0xFF;
            dst[i * 4 + 3] = self->shm_format == WL_SHM_FORMAT_ARGB8888 ? ((pixel >> 24) & 0xFF) : 0xFF;
        }
    }

    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->texture_id);
    self->params.egl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, self->capture_size.x, self->capture_size.y, GL_RGBA, GL_UNSIGNED_BYTE, self->rgba_data);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    self->frame_ready = false;
    self->has_frame = true;
}

static void session_handle_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t width, uint32_t height) {
    (void)session;
    gsr_capture_wayland_window *self = data;
    if(self->capture_size.x != (int)width || self->capture_size.y != (int)height) {
        self->capture_size.x = width;
        self->capture_size.y = height;
        self->clear_next_frame = true;
    }
}

static void session_handle_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t format) {
    (void)session;
    gsr_capture_wayland_window *self = data;
    if(format == WL_SHM_FORMAT_ARGB8888)
        self->supports_argb8888 = true;
    else if(format == WL_SHM_FORMAT_XRGB8888)
        self->supports_xrgb8888 = true;
    else if(format == WL_SHM_FORMAT_BGR888)
        self->supports_bgr888 = true;
    else if(format == WL_SHM_FORMAT_RGB888)
        self->supports_rgb888 = true;
}

static void session_handle_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session, struct wl_array *device) {
    (void)data;
    (void)session;
    (void)device;
}

static void session_handle_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t format, struct wl_array *modifiers) {
    (void)data;
    (void)session;
    (void)format;
    (void)modifiers;
}

static void session_handle_done(void *data, struct ext_image_copy_capture_session_v1 *session) {
    (void)session;
    gsr_capture_wayland_window *self = data;
    self->constraints_done = true;
}

static void session_handle_stopped(void *data, struct ext_image_copy_capture_session_v1 *session) {
    (void)session;
    gsr_capture_wayland_window *self = data;
    self->session_stopped = true;
    self->should_stop = true;
    self->stop_is_error = false;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_handle_buffer_size,
    .shm_format = session_handle_shm_format,
    .dmabuf_device = session_handle_dmabuf_device,
    .dmabuf_format = session_handle_dmabuf_format,
    .done = session_handle_done,
    .stopped = session_handle_stopped,
};

static bool capture_wait_for_constraints(gsr_capture_wayland_window *self) {
    const double start = clock_get_monotonic_seconds();
    while(!self->constraints_done && !self->session_stopped && clock_get_monotonic_seconds() - start < 5.0)
        wl_display_roundtrip(self->wl.display);

    if(!self->constraints_done) {
        fprintf(stderr, "gsr error: wayland window capture: timed out waiting for capture constraints\n");
        return false;
    }

    if(self->capture_size.x <= 0 || self->capture_size.y <= 0) {
        fprintf(stderr, "gsr error: wayland window capture: invalid capture size %dx%d\n", self->capture_size.x, self->capture_size.y);
        return false;
    }

    if(!self->supports_argb8888 && !self->supports_xrgb8888 && !self->supports_bgr888 && !self->supports_rgb888) {
        fprintf(stderr, "gsr error: wayland window capture: compositor didn't provide a supported wl_shm format\n");
        return false;
    }

    return true;
}

static void capture_stop(gsr_capture_wayland_window *self) {
    capture_destroy_frame(self);

    if(self->session) {
        ext_image_copy_capture_session_v1_destroy(self->session);
        self->session = NULL;
    }

    if(self->source) {
        ext_image_capture_source_v1_destroy(self->source);
        self->source = NULL;
    }

    capture_destroy_buffer(self);

    if(self->texture_id) {
        self->params.egl->glDeleteTextures(1, &self->texture_id);
        self->texture_id = 0;
    }

    wayland_window_context_deinit(&self->wl);
    self->selected_toplevel = NULL;
}

static int capture_start(gsr_capture *cap, gsr_capture_metadata *capture_metadata) {
    gsr_capture_wayland_window *self = cap->priv;

    if(self->params.match_type == GSR_WAYLAND_WINDOW_MATCH_TYPE_NONE || !self->params.match_value || self->params.match_value[0] == '\0') {
        fprintf(stderr, "gsr error: wayland window capture requires window:id=..., window:app_id=... or window:title=...\n");
        return -1;
    }

    if(!wayland_window_context_init(&self->wl, self->params.egl->window))
        return -1;

    if(!wayland_window_context_has_capture_support(&self->wl)) {
        fprintf(stderr, "gsr error: wayland window capture is not supported by this compositor. Required protocols: %s, %s, %s and wl_shm. Use -w portal or update/change compositor if needed\n",
            ext_foreign_toplevel_list_v1_interface.name,
            ext_foreign_toplevel_image_capture_source_manager_v1_interface.name,
            ext_image_copy_capture_manager_v1_interface.name);
        return -1;
    }

    if(!wait_for_matching_toplevel(&self->wl, self->params.match_type, self->params.match_value, self->params.wait_timeout_sec, &self->selected_toplevel))
        return -1;

    self->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(self->wl.toplevel_source_manager, self->selected_toplevel->handle);
    if(!self->source) {
        fprintf(stderr, "gsr error: wayland window capture: failed to create toplevel capture source\n");
        return -1;
    }

    const uint32_t options = self->params.record_cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
    self->session = ext_image_copy_capture_manager_v1_create_session(self->wl.copy_manager, self->source, options);
    if(!self->session) {
        fprintf(stderr, "gsr error: wayland window capture: failed to create capture session\n");
        return -1;
    }

    ext_image_copy_capture_session_v1_add_listener(self->session, &session_listener, self);
    wl_display_roundtrip(self->wl.display);

    if(!capture_wait_for_constraints(self))
        return -1;

    if(!capture_create_buffer(self))
        return -1;

    self->texture_id = gl_create_texture(self->params.egl, self->capture_size.x, self->capture_size.y, GL_RGBA8, GL_RGBA, GL_LINEAR);
    if(!self->texture_id) {
        fprintf(stderr, "gsr error: wayland window capture: failed to create input texture\n");
        return -1;
    }

    capture_queue_frame(self);
    const double first_frame_start = clock_get_monotonic_seconds();
    while(!self->frame_ready && !self->should_stop && clock_get_monotonic_seconds() - first_frame_start < 5.0)
        wl_display_roundtrip(self->wl.display);

    if(!self->frame_ready) {
        fprintf(stderr, "gsr error: wayland window capture: timed out waiting for first frame\n");
        return -1;
    }

    capture_upload_ready_frame(self);
    capture_queue_frame(self);

    if(self->params.output_resolution.x == 0 && self->params.output_resolution.y == 0) {
        capture_metadata->video_size = self->capture_size;
    } else {
        self->params.output_resolution = scale_keep_aspect_ratio(self->capture_size, self->params.output_resolution);
        capture_metadata->video_size = self->params.output_resolution;
    }

    self->damaged = true;
    return 0;
}

static void capture_tick(gsr_capture *cap) {
    gsr_capture_wayland_window *self = cap->priv;
    if(self->should_stop)
        return;

    wl_display_dispatch_pending(self->wl.display);
    wl_display_flush(self->wl.display);

    if(self->selected_toplevel && self->selected_toplevel->closed) {
        self->should_stop = true;
        self->stop_is_error = false;
    }
}

static bool capture_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_wayland_window *self = cap->priv;
    if(self->should_stop) {
        *err = self->stop_is_error;
        return true;
    }

    return false;
}

static void capture_pre_capture(gsr_capture *cap, gsr_capture_metadata *capture_metadata, gsr_color_conversion *color_conversion) {
    (void)capture_metadata;
    gsr_capture_wayland_window *self = cap->priv;
    self->do_capture = self->has_frame;

    if(self->should_stop)
        return;

    if(self->constraints_done && self->clear_next_frame) {
        self->clear_next_frame = false;
        color_conversion->schedule_clear = true;

        capture_destroy_frame(self);
        capture_destroy_buffer(self);
        if(!capture_create_buffer(self)) {
            self->should_stop = true;
            self->stop_is_error = true;
            return;
        }

        if(self->texture_id)
            self->params.egl->glDeleteTextures(1, &self->texture_id);
        self->texture_id = gl_create_texture(self->params.egl, self->capture_size.x, self->capture_size.y, GL_RGBA8, GL_RGBA, GL_LINEAR);
        if(!self->texture_id) {
            self->should_stop = true;
            self->stop_is_error = true;
            return;
        }

        self->has_frame = false;
        capture_queue_frame(self);
        return;
    }

    if(!self->frame_ready)
        return;

    capture_upload_ready_frame(self);
    self->do_capture = true;
    capture_queue_frame(self);
}

static int capture_capture(gsr_capture *cap, gsr_capture_metadata *capture_metadata, gsr_color_conversion *color_conversion) {
    gsr_capture_wayland_window *self = cap->priv;
    if(self->should_stop || !self->do_capture)
        return -1;

    const vec2i output_size = scale_keep_aspect_ratio(self->capture_size, capture_metadata->recording_size);
    const vec2i target_pos = gsr_capture_get_target_position(output_size, capture_metadata);

    gsr_color_conversion_draw(color_conversion, self->texture_id,
        target_pos, output_size,
        (vec2i){0, 0}, self->capture_size, self->capture_size,
        wl_transform_to_gsr_rotation(self->frame_transform), capture_metadata->flip, GSR_SOURCE_COLOR_RGB, false);

    return 0;
}

static bool capture_is_damaged(gsr_capture *cap) {
    gsr_capture_wayland_window *self = cap->priv;
    return self->damaged || self->frame_ready;
}

static void capture_clear_damage(gsr_capture *cap) {
    gsr_capture_wayland_window *self = cap->priv;
    self->damaged = false;
}

static void capture_destroy(gsr_capture *cap) {
    gsr_capture_wayland_window *self = cap->priv;
    if(cap->started)
        capture_stop(self);
    free(self);
    free(cap);
}

gsr_capture* gsr_capture_wayland_window_create(const gsr_capture_wayland_window_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_wayland_window_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_wayland_window *cap_wayland = calloc(1, sizeof(gsr_capture_wayland_window));
    if(!cap_wayland) {
        free(cap);
        return NULL;
    }

    cap_wayland->params = *params;
    cap_wayland->frame_transform = WL_OUTPUT_TRANSFORM_NORMAL;

    *cap = (gsr_capture) {
        .start = capture_start,
        .tick = capture_tick,
        .should_stop = capture_should_stop,
        .pre_capture = capture_pre_capture,
        .capture = capture_capture,
        .is_damaged = capture_is_damaged,
        .clear_damage = capture_clear_damage,
        .destroy = capture_destroy,
        .priv = cap_wayland
    };

    return cap;
}
