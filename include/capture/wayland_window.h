#ifndef GSR_CAPTURE_WAYLAND_WINDOW_H
#define GSR_CAPTURE_WAYLAND_WINDOW_H

#include "capture.h"

typedef struct gsr_window gsr_window;

typedef enum {
    GSR_WAYLAND_WINDOW_MATCH_TYPE_NONE,
    GSR_WAYLAND_WINDOW_MATCH_TYPE_IDENTIFIER,
    GSR_WAYLAND_WINDOW_MATCH_TYPE_APP_ID,
    GSR_WAYLAND_WINDOW_MATCH_TYPE_TITLE
} gsr_wayland_window_match_type;

typedef struct {
    const char *identifier;
    const char *app_id;
    const char *title;
} gsr_wayland_window_info;

typedef void (*gsr_wayland_window_list_callback)(const gsr_wayland_window_info *info, void *userdata);

typedef struct {
    gsr_egl *egl;
    gsr_wayland_window_match_type match_type;
    const char *match_value;
    double wait_timeout_sec;
    bool record_cursor;
    vec2i output_resolution;
} gsr_capture_wayland_window_params;

bool gsr_capture_wayland_window_supported(gsr_window *window);
bool gsr_capture_wayland_window_list(gsr_window *window, gsr_wayland_window_list_callback callback, void *userdata);
gsr_capture* gsr_capture_wayland_window_create(const gsr_capture_wayland_window_params *params);

#endif /* GSR_CAPTURE_WAYLAND_WINDOW_H */
