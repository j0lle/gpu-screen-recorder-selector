#ifndef GSR_CAPTURE_V4L2_H
#define GSR_CAPTURE_V4L2_H

#include "capture.h"

typedef enum {
    GSR_CAPTURE_V4L2_PIXFMT_AUTO,
    GSR_CAPTURE_V4L2_PIXFMT_YUYV,
    GSR_CAPTURE_V4L2_PIXFMT_MJPEG
} gsr_capture_v4l2_pixfmt;

typedef struct {
    bool yuyv;
    bool mjpeg;
} gsr_capture_v4l2_supported_pixfmts;

typedef struct {
    gsr_egl *egl;
    vec2i output_resolution;
    const char *device_path;
    gsr_capture_v4l2_pixfmt pixfmt;
    int fps;
} gsr_capture_v4l2_params;

gsr_capture* gsr_capture_v4l2_create(const gsr_capture_v4l2_params *params);

typedef void (*v4l2_devices_query_callback)(const char *path, gsr_capture_v4l2_supported_pixfmts supported_pixfmts, vec2i size, void *userdata);
void gsr_capture_v4l2_list_devices(v4l2_devices_query_callback callback, void *userdata);

#endif /* GSR_CAPTURE_V4L2_H */
