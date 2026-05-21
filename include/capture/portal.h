#ifndef GSR_CAPTURE_PORTAL_H
#define GSR_CAPTURE_PORTAL_H

#include "capture.h"

typedef struct {
    gsr_egl *egl;
    bool record_cursor;
    bool restore_portal_session;
    /* 0 means all supported source types */
    uint32_t capture_type;
    /* If this is set to NULL then this defaults to $XDG_CONFIG_HOME/gpu-screen-recorder/restore_token ($XDG_CONFIG_HOME defaults to $HOME/.config) */
    const char *portal_session_token_filepath;
    vec2i output_resolution;
} gsr_capture_portal_params;

gsr_capture* gsr_capture_portal_create(const gsr_capture_portal_params *params);

#endif /* GSR_CAPTURE_PORTAL_H */
