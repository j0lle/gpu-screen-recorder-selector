#include "../../include/capture/v4l2.h"
#include "../../include/color_conversion.h"
#include "../../include/egl.h"
#include "../../include/utils.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#define TJPF_RGB 0
#define TJPF_RGBA 7
#define TJFLAG_FASTDCT  2048

#define NUM_BUFFERS 2
#define NUM_PBOS 2

typedef void* tjhandle;
typedef tjhandle (*FUNC_tjInitDecompress)(void);
typedef int (*FUNC_tjDestroy)(tjhandle handle);
typedef int (*FUNC_tjDecompressHeader2)(tjhandle handle,
                                        unsigned char *jpegBuf, unsigned long jpegSize,
                                        int *width, int *height, int *jpegSubsamp);
typedef int (*FUNC_tjDecompress2)(tjhandle handle, const unsigned char *jpegBuf,
                                  unsigned long jpegSize, unsigned char *dstBuf,
                                  int width, int pitch, int height, int pixelFormat,
                                  int flags);
typedef char* (*FUNC_tjGetErrorStr2)(tjhandle handle);

typedef enum {
    V4L2_BUFFER_TYPE_DMABUF,
    V4L2_BUFFER_TYPE_MMAP
} v4l2_buffer_type;

typedef struct {
    gsr_capture_v4l2_params params;
    vec2i capture_size;

    bool should_stop;
    bool stop_is_error;

    int fd;
    int dmabuf_fd[NUM_BUFFERS];
    EGLImage dma_image[NUM_BUFFERS];
    unsigned int texture_id[NUM_BUFFERS];
    unsigned int prev_texture_index;
    bool got_first_frame;

    void *dmabuf_map[NUM_BUFFERS];
    size_t dmabuf_size[NUM_BUFFERS];
    unsigned int pbos[NUM_PBOS];
    unsigned int pbo_index;
    
    v4l2_buffer_type buffer_type;

    void *libturbojpeg_lib;
    FUNC_tjInitDecompress tjInitDecompress;
    FUNC_tjDestroy tjDestroy;
    FUNC_tjDecompressHeader2 tjDecompressHeader2;
    FUNC_tjDecompress2 tjDecompress2;
    FUNC_tjGetErrorStr2 tjGetErrorStr2;
    tjhandle jpeg_decompressor;

    double capture_start_time;

    bool yuyv_conversion_fallback;
} gsr_capture_v4l2;

static int xioctl(int fd, unsigned long request, void *arg) {
    int r;

    do {
        r = ioctl(fd, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static void gsr_capture_v4l2_stop(gsr_capture_v4l2 *self) {
    self->params.egl->glDeleteBuffers(NUM_PBOS, self->pbos);
    for(int i = 0; i < NUM_PBOS; ++i) {
        self->pbos[i] = 0;
    }

    self->params.egl->glDeleteTextures(NUM_BUFFERS, self->texture_id);
    for(int i = 0; i < NUM_BUFFERS; ++i) {
        self->texture_id[i] = 0;
    }

    for(int i = 0; i < NUM_BUFFERS; ++i) {
        if(self->dmabuf_map[i]) {
            munmap(self->dmabuf_map[i], self->dmabuf_size[i]);
            self->dmabuf_map[i] = NULL;
        }

        if(self->dma_image[i]) {
            self->params.egl->eglDestroyImage(self->params.egl->egl_display, self->dma_image[i]);
            self->dma_image[i] = NULL;
        }

        if(self->dmabuf_fd[i] > 0) {
            close(self->dmabuf_fd[i]);
            self->dmabuf_fd[i] = 0;
        }
    }

    if(self->fd > 0) {
        xioctl(self->fd, VIDIOC_STREAMOFF, &(enum v4l2_buf_type){V4L2_BUF_TYPE_VIDEO_CAPTURE});
        close(self->fd);
        self->fd = 0;
    }

    if(self->jpeg_decompressor) {
        self->tjDestroy(self->jpeg_decompressor);
        self->jpeg_decompressor = NULL;
    }

    if(self->libturbojpeg_lib) {
        dlclose(self->libturbojpeg_lib);
        self->libturbojpeg_lib = NULL;
    }
}

static void gsr_capture_v4l2_reset_cropping(gsr_capture_v4l2 *self) {
    struct v4l2_cropcap cropcap = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE
    };
    if(xioctl(self->fd, VIDIOC_CROPCAP, &cropcap) == 0) {
        struct v4l2_crop crop = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .c = cropcap.defrect /* reset to default */
        };

        if(xioctl(self->fd, VIDIOC_S_CROP, &crop) == -1) {
            switch (errno) {
            case EINVAL:
                /* Cropping not supported. */
                break;
            default:
                /* Errors ignored. */
                break;
            }
        }
    } else {
        /* Errors ignored. */
    }
}

gsr_capture_v4l2_supported_pixfmts gsr_capture_v4l2_get_supported_pixfmts(int fd) {
    gsr_capture_v4l2_supported_pixfmts result = {0};

    struct v4l2_fmtdesc fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE
    };
    while(xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        //fprintf(stderr, "fmt: %d, desc: %s, flags: %d\n", fmt.pixelformat, fmt.description, fmt.flags);
        switch(fmt.pixelformat) {
            case V4L2_PIX_FMT_YUYV:
                result.yuyv = true;
                break;
            case V4L2_PIX_FMT_MJPEG:
                result.mjpeg = true;
                break;
        }
        ++fmt.index;
    }

    return result;
}

static uint32_t gsr_pixfmt_to_v4l2_pixfmt(gsr_capture_v4l2_pixfmt pixfmt) {
    switch(pixfmt) {
        case GSR_CAPTURE_V4L2_PIXFMT_AUTO:
            assert(false);
            break;
        case GSR_CAPTURE_V4L2_PIXFMT_YUYV:
            return V4L2_PIX_FMT_YUYV;
        case GSR_CAPTURE_V4L2_PIXFMT_MJPEG:
            return V4L2_PIX_FMT_MJPEG;
    }
    assert(false);
    return V4L2_PIX_FMT_YUYV;
}

static bool gsr_capture_v4l2_validate_pixfmt(gsr_capture_v4l2 *self, const gsr_capture_v4l2_supported_pixfmts supported_pixfmts) {
    switch(self->params.pixfmt) {
        case GSR_CAPTURE_V4L2_PIXFMT_AUTO: {
            if(supported_pixfmts.yuyv) {
                self->params.pixfmt = GSR_CAPTURE_V4L2_PIXFMT_YUYV;
            } else if(supported_pixfmts.mjpeg) {
                self->params.pixfmt = GSR_CAPTURE_V4L2_PIXFMT_MJPEG;
            } else {
                fprintf(stderr, "gsr error: gsr_capture_v4l2_create: %s doesn't support yuyv nor mjpeg. GPU Screen Recorder supports only yuyv and mjpeg at the moment. Report this as an issue, see: https://git.dec05eba.com/?p=about\n", self->params.device_path);
                return false;
            }
            break;
        }
        case GSR_CAPTURE_V4L2_PIXFMT_YUYV: {
            if(!supported_pixfmts.yuyv) {
                fprintf(stderr, "gsr error: gsr_capture_v4l2_create: %s doesn't support yuyv. Try recording with -pixfmt mjpeg or -pixfmt auto instead\n", self->params.device_path);
                return false;
            }
            break;
        }
        case GSR_CAPTURE_V4L2_PIXFMT_MJPEG: {
            if(!supported_pixfmts.mjpeg) {
                fprintf(stderr, "gsr error: gsr_capture_v4l2_create: %s doesn't support mjpeg. Try recording with -pixfmt yuyv or -pixfmt auto instead\n", self->params.device_path);
                return false;
            }
            break;
        }
    }
    return true;
}

static bool gsr_capture_v4l2_create_pbos(gsr_capture_v4l2 *self, int width, int height) {
    self->pbo_index = 0;

    self->params.egl->glGenBuffers(NUM_PBOS, self->pbos);
    for(int i = 0; i < NUM_PBOS; ++i) {
        if(self->pbos[i] == 0) {
            fprintf(stderr, "gsr error: gsr_capture_v4l2_create_pbos: failed to create pixel buffer objects\n");
            return false;
        }

        self->params.egl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, self->pbos[i]);
        self->params.egl->glBufferData(GL_PIXEL_UNPACK_BUFFER, width * height * 4, 0, GL_DYNAMIC_DRAW);
    }

    self->params.egl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return true;
}

static bool gsr_capture_v4l2_map_buffer(gsr_capture_v4l2 *self, const struct v4l2_format *fmt) {
    switch(self->params.pixfmt) {
        case GSR_CAPTURE_V4L2_PIXFMT_AUTO: {
            assert(false);
            return false;
        }
        case GSR_CAPTURE_V4L2_PIXFMT_YUYV: {
            self->params.egl->glGenTextures(NUM_BUFFERS, self->texture_id);

            for(int i = 0; i < NUM_BUFFERS; ++i) {
                self->dma_image[i] = self->params.egl->eglCreateImage(self->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, (intptr_t[]) {
                    EGL_WIDTH, fmt->fmt.pix.width,
                    EGL_HEIGHT, fmt->fmt.pix.height,
                    EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_YUYV,
                    EGL_DMA_BUF_PLANE0_FD_EXT, self->dmabuf_fd[i],
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, fmt->fmt.pix.bytesperline,
                    EGL_NONE
                });

                if(!self->dma_image[i]) {
                    self->yuyv_conversion_fallback = true;
                    self->dma_image[i] = self->params.egl->eglCreateImage(self->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, (intptr_t[]) {
                        EGL_WIDTH, fmt->fmt.pix.width,
                        EGL_HEIGHT, fmt->fmt.pix.height,
                        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_RG88,
                        EGL_DMA_BUF_PLANE0_FD_EXT, self->dmabuf_fd[i],
                        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                        EGL_DMA_BUF_PLANE0_PITCH_EXT, fmt->fmt.pix.bytesperline,
                        EGL_NONE
                    });

                    if(!self->dma_image[i]) {
                        fprintf(stderr, "gsr error: gsr_capture_v4l2_map_buffer: eglCreateImage failed, error: %d\n", self->params.egl->eglGetError());
                        return false;
                    }
                }

                self->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, self->texture_id[i]);
                self->params.egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, self->dma_image[i]);
                self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                self->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
                if(self->texture_id[i] == 0) {
                    fprintf(stderr, "gsr error: gsr_capture_v4l2_map_buffer: failed to create texture\n");
                    return false;
                }
            }

            self->buffer_type = V4L2_BUFFER_TYPE_DMABUF;
            break;
        }
        case GSR_CAPTURE_V4L2_PIXFMT_MJPEG: {
            for(int i = 0; i < NUM_BUFFERS; ++i) {
                self->dmabuf_size[i] = fmt->fmt.pix.sizeimage;
                self->dmabuf_map[i] = mmap(NULL, fmt->fmt.pix.sizeimage, PROT_READ, MAP_SHARED, self->dmabuf_fd[i], 0);
                if(self->dmabuf_map[i] == MAP_FAILED) {
                    fprintf(stderr, "gsr error: gsr_capture_v4l2_map_buffer: mmap failed, error: %s\n", strerror(errno));
                    return false;
                }

                // GL_RGBA is intentionally used here instead of GL_RGB, because the performance is much better when using glTexSubImage2D (22% cpu usage compared to 38% cpu usage)
                self->texture_id[i] = gl_create_texture(self->params.egl, fmt->fmt.pix.width, fmt->fmt.pix.height, GL_RGBA8, GL_RGBA, GL_LINEAR);
                if(self->texture_id[i] == 0) {
                    fprintf(stderr, "gsr error: gsr_capture_v4l2_map_buffer: failed to create texture\n");
                    return false;
                }
            }

            if(!gsr_capture_v4l2_create_pbos(self, fmt->fmt.pix.width, fmt->fmt.pix.height))
                return false;

            self->buffer_type = V4L2_BUFFER_TYPE_MMAP;
            break;
        }
    }
    return true;
}

static int gsr_capture_v4l2_setup(gsr_capture_v4l2 *self) {
    self->fd = open(self->params.device_path, O_RDWR | O_NONBLOCK);
    if(self->fd < 0) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_create: failed to open %s, error: %s\n", self->params.device_path, strerror(errno));
        return -1;
    }

    struct v4l2_capability cap = {0};
    if(xioctl(self->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        if(EINVAL == errno) {
            fprintf(stderr, "gsr error: gsr_capture_v4l2_create: %s isn't a v4l2 device\n", self->params.device_path);
            return -1;
        } else {
            fprintf(stderr, "gsr error: gsr_capture_v4l2_create: VIDIOC_QUERYCAP failed, error: %s\n", strerror(errno));
            return -1;
        }
    }

    if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_create: %s isn't a video capture device\n", self->params.device_path);
        return -1;
    }

    if(!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_create: %s doesn't support streaming i/o\n", self->params.device_path);
        return -1;
    }

    gsr_capture_v4l2_reset_cropping(self);

    const gsr_capture_v4l2_supported_pixfmts supported_pixfmts = gsr_capture_v4l2_get_supported_pixfmts(self->fd);
    if(!gsr_capture_v4l2_validate_pixfmt(self, supported_pixfmts))
        return -1;

    if(self->params.pixfmt == GSR_CAPTURE_V4L2_PIXFMT_MJPEG) {
        dlerror(); /* clear */
        self->libturbojpeg_lib = dlopen("libturbojpeg.so.0", RTLD_LAZY);
        if(!self->libturbojpeg_lib) {
            fprintf(stderr, "gsr error: gsr_capture_v4l2_create: failed to load libturbojpeg.so.0 which is required for camera mjpeg capture, error: %s\n", dlerror());
            return -1;
        }

        self->tjInitDecompress = (FUNC_tjInitDecompress)dlsym(self->libturbojpeg_lib, "tjInitDecompress");
        self->tjDestroy = (FUNC_tjDestroy)dlsym(self->libturbojpeg_lib, "tjDestroy");
        self->tjDecompressHeader2 = (FUNC_tjDecompressHeader2)dlsym(self->libturbojpeg_lib, "tjDecompressHeader2");
        self->tjDecompress2 = (FUNC_tjDecompress2)dlsym(self->libturbojpeg_lib, "tjDecompress2");
        self->tjGetErrorStr2 = (FUNC_tjGetErrorStr2)dlsym(self->libturbojpeg_lib, "tjGetErrorStr2");

        if(!self->tjInitDecompress || !self->tjDestroy || !self->tjDecompressHeader2 || !self->tjDecompress2 || !self->tjGetErrorStr2) {
            fprintf(stderr, "gsr error: gsr_capture_v4l2_create: libturbojpeg.so.0 is missing functions. The libturbojpeg version installed on your system might be outdated\n");
            return -1;
        }

        self->jpeg_decompressor = self->tjInitDecompress();
        if(!self->jpeg_decompressor) {
            fprintf(stderr, "gsr error: gsr_capture_v4l2_create: failed to create jpeg decompressor\n");
            return -1;
        }
    }

    const uint32_t v4l2_pixfmt = gsr_pixfmt_to_v4l2_pixfmt(self->params.pixfmt);
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.pixelformat = v4l2_pixfmt
    };
    if(xioctl(self->fd, VIDIOC_S_FMT, &fmt) == -1) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_create: VIDIOC_S_FMT failed, error: %s\n", strerror(errno));
        return -1;
    }

    if(fmt.fmt.pix.pixelformat != v4l2_pixfmt) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_create: pixel format isn't as requested (got pixel format: %u, requested: %u), error: %s\n", fmt.fmt.pix.pixelformat, v4l2_pixfmt, strerror(errno));
        return -1;
    }

    self->capture_size.x = fmt.fmt.pix.width;
    self->capture_size.y = fmt.fmt.pix.height;

    struct v4l2_requestbuffers reqbuf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .count = NUM_BUFFERS
    };
    if(xioctl(self->fd, VIDIOC_REQBUFS, &reqbuf) == -1) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_create: VIDIOC_REQBUFS failed, error: %s\n", strerror(errno));
        return -1;
    }

    for(int i = 0; i < NUM_BUFFERS; ++i) {
        struct v4l2_exportbuffer expbuf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .index = i,
            .flags = O_RDONLY
        };
        if(xioctl(self->fd, VIDIOC_EXPBUF, &expbuf) == -1) {
            fprintf(stderr, "gsr error: gsr_capture_v4l2_create: VIDIOC_EXPBUF failed, error: %s\n", strerror(errno));
            return -1;
        }
        self->dmabuf_fd[i] = expbuf.fd;
    }

    if(!gsr_capture_v4l2_map_buffer(self, &fmt))
        return -1;

    for(int i = 0; i < NUM_BUFFERS; ++i) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .index = i,
            .memory = V4L2_MEMORY_MMAP
        };
        xioctl(self->fd, VIDIOC_QBUF, &buf);
    }

    if(xioctl(self->fd, VIDIOC_STREAMON, &(enum v4l2_buf_type){V4L2_BUF_TYPE_VIDEO_CAPTURE})) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_create: VIDIOC_STREAMON failed, error: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "gsr info: gsr_capture_v4l2_create: waiting for camera %s to be ready\n", self->params.device_path);
    return 0;
}

static int gsr_capture_v4l2_start(gsr_capture *cap, gsr_capture_metadata *capture_metadata) {
    gsr_capture_v4l2 *self = cap->priv;

    const int result = gsr_capture_v4l2_setup(self);
    if(result != 0) {
        gsr_capture_v4l2_stop(self);
        return result;
    }

    if(self->params.output_resolution.x == 0 && self->params.output_resolution.y == 0) {
        capture_metadata->video_size = self->capture_size;
    } else {
        self->params.output_resolution = scale_keep_aspect_ratio(self->capture_size, self->params.output_resolution);
        capture_metadata->video_size = self->params.output_resolution;
    }

    self->capture_start_time = clock_get_monotonic_seconds();
    return 0;
}

static void gsr_capture_v4l2_tick(gsr_capture *cap) {
    gsr_capture_v4l2 *self = cap->priv;
    if(!self->got_first_frame && !self->should_stop) {
        const double timeout_sec = 5.0;
        if(clock_get_monotonic_seconds() - self->capture_start_time >= timeout_sec) {
            fprintf(stderr, "gsr error: gsr_capture_v4l2_capture: didn't receive camera data in %f seconds\n", timeout_sec);
            self->should_stop = true;
            self->stop_is_error = true;
        }
    }
}

static void gsr_capture_v4l2_decode_jpeg_to_texture(gsr_capture_v4l2 *self, const struct v4l2_buffer *buf) {
    int jpeg_subsamp = 0;
    int jpeg_width = 0;
    int jpeg_height = 0;
    if(self->tjDecompressHeader2(self->jpeg_decompressor, self->dmabuf_map[buf->index], buf->bytesused, &jpeg_width, &jpeg_height, &jpeg_subsamp) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_capture: failed to decompress camera jpeg header data, error: %s\n", self->tjGetErrorStr2(self->jpeg_decompressor));
        return;
    }

    if(jpeg_width != self->capture_size.x || jpeg_height != self->capture_size.y) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_capture: got jpeg data of incorrect dimensions. Expected %dx%d, got %dx%d\n", self->capture_size.x, self->capture_size.y, jpeg_width, jpeg_height);
        return;
    }

    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->texture_id[buf->index]);

    self->pbo_index = (self->pbo_index + 1) % NUM_PBOS;
    const unsigned int next_pbo_index = (self->pbo_index + 1) % NUM_PBOS;
    
    self->params.egl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, self->pbos[self->pbo_index]);
    self->params.egl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, self->capture_size.x, self->capture_size.y, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    self->params.egl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, self->pbos[next_pbo_index]);
    self->params.egl->glBufferData(GL_PIXEL_UNPACK_BUFFER, self->capture_size.x * self->capture_size.y * 4, 0, GL_DYNAMIC_DRAW);

    void *mapped_buffer = self->params.egl->glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, self->capture_size.x * self->capture_size.y * 4, GL_MAP_WRITE_BIT);
    if(mapped_buffer) {
        if(self->tjDecompress2(self->jpeg_decompressor, self->dmabuf_map[buf->index], buf->bytesused, mapped_buffer, jpeg_width, 0, jpeg_height, TJPF_RGBA, TJFLAG_FASTDCT) != 0)
            fprintf(stderr, "gsr error: gsr_capture_v4l2_capture: failed to decompress camera jpeg data, error: %s\n", self->tjGetErrorStr2(self->jpeg_decompressor));
        self->params.egl->glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    }

    self->params.egl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);
}

static int gsr_capture_v4l2_capture(gsr_capture *cap, gsr_capture_metadata *capture_metadata, gsr_color_conversion *color_conversion) {
    (void)color_conversion;
    gsr_capture_v4l2 *self = cap->priv;

    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP
    };

    xioctl(self->fd, VIDIOC_DQBUF, &buf);
    unsigned int texture_index = buf.index;

    if(buf.bytesused > 0 && !(buf.flags & V4L2_BUF_FLAG_ERROR)) {
        if(!self->got_first_frame)
            fprintf(stderr, "gsr info: gsr_capture_v4l2_capture: camera %s is now ready\n", self->params.device_path);
        self->got_first_frame = true;

        switch(self->buffer_type) {
            case V4L2_BUFFER_TYPE_DMABUF: {
                //self->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, self->texture_id);
                //self->params.egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, self->dma_image[buf.index]);
                //self->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
                break;
            }
            case V4L2_BUFFER_TYPE_MMAP: {
                //xioctl(self->dmabuf_fd[buf.index], DMA_BUF_IOCTL_SYNC, &(struct dma_buf_sync){ .flags = DMA_BUF_SYNC_START });
                gsr_capture_v4l2_decode_jpeg_to_texture(self, &buf);
                //xioctl(self->dmabuf_fd[buf.index], DMA_BUF_IOCTL_SYNC, &(struct dma_buf_sync){ .flags = DMA_BUF_SYNC_END });
                break;
            }
        }

        self->prev_texture_index = buf.index;
    } else {
        texture_index = self->prev_texture_index;
    }
    xioctl(self->fd, VIDIOC_QBUF, &buf);

    const vec2i output_size = scale_keep_aspect_ratio(self->capture_size, capture_metadata->recording_size);
    const vec2i target_pos = gsr_capture_get_target_position(output_size, capture_metadata);

    self->params.egl->glFlush();
    // TODO: Use the minimal barrier required
    self->params.egl->glMemoryBarrier(GL_ALL_BARRIER_BITS);
    // TODO: Remove this?
    if(self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA)
        self->params.egl->glFinish();

    if(self->buffer_type == V4L2_BUFFER_TYPE_DMABUF) {
        gsr_color_conversion_draw(color_conversion, self->texture_id[texture_index],
            target_pos, output_size,
            (vec2i){0, 0}, self->capture_size, self->capture_size,
            GSR_ROT_0, capture_metadata->flip, self->yuyv_conversion_fallback ? GSR_SOURCE_COLOR_YUYV : GSR_SOURCE_COLOR_RGB, true);
    } else {
        gsr_color_conversion_draw(color_conversion, self->texture_id[texture_index],
            target_pos, output_size,
            (vec2i){0, 0}, self->capture_size, self->capture_size,
            GSR_ROT_0, capture_metadata->flip, GSR_SOURCE_COLOR_RGB, false);
    }

    return self->got_first_frame ? 0 : -1;
}

static bool gsr_capture_v4l2_uses_external_image(gsr_capture *cap) {
    (void)cap;
    return true;
}

static bool gsr_capture_v4l2_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_v4l2 *self = cap->priv;
    if(err)
        *err = self->stop_is_error;
    return self->should_stop;
}

static bool gsr_capture_v4l2_is_damaged(gsr_capture *cap) {
    gsr_capture_v4l2 *self = cap->priv;
    struct pollfd poll_data = {
        .fd = self->fd,
        .events = POLLIN,
        .revents = 0
    };
    return poll(&poll_data, 1, 0) > 0 && (poll_data.revents & POLLIN);
}

static void gsr_capture_v4l2_clear_damage(gsr_capture *cap) {
    gsr_capture_v4l2 *self = cap->priv;
    (void)self;
}

static void gsr_capture_v4l2_destroy(gsr_capture *cap) {
    gsr_capture_v4l2 *self = cap->priv;
    if(cap->priv) {
        gsr_capture_v4l2_stop(self);
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_v4l2_create(const gsr_capture_v4l2_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_v4l2_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_v4l2 *cap_camera = calloc(1, sizeof(gsr_capture_v4l2));
    if(!cap_camera) {
        free(cap);
        return NULL;
    }

    cap_camera->params = *params;

    *cap = (gsr_capture) {
        .start = gsr_capture_v4l2_start,
        .tick = gsr_capture_v4l2_tick,
        .should_stop = gsr_capture_v4l2_should_stop,
        .capture = gsr_capture_v4l2_capture,
        .uses_external_image = gsr_capture_v4l2_uses_external_image,
        .is_damaged = gsr_capture_v4l2_is_damaged,
        .clear_damage = gsr_capture_v4l2_clear_damage,
        .destroy = gsr_capture_v4l2_destroy,
        .priv = cap_camera
    };

    return cap;
}

void gsr_capture_v4l2_list_devices(v4l2_devices_query_callback callback, void *userdata) {
    void *libturbojpeg_lib = dlopen("libturbojpeg.so.0", RTLD_LAZY);
    const bool has_libturbojpeg_lib = libturbojpeg_lib != NULL;
    if(libturbojpeg_lib)
        dlclose(libturbojpeg_lib);

    char v4l2_device_path[128];
    for(int i = 0; i < 8; ++i) {
        snprintf(v4l2_device_path, sizeof(v4l2_device_path), "/dev/video%d", i);

        const int fd = open(v4l2_device_path, O_RDWR | O_NONBLOCK);
        if(fd < 0)
            continue;

        struct v4l2_capability cap = {0};
        if(xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1)
            goto next;

        if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
            goto next;

        if(!(cap.capabilities & V4L2_CAP_STREAMING))
            goto next;

        struct v4l2_format fmt = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        };
        if(xioctl(fd, VIDIOC_G_FMT, &fmt) == -1)
            goto next;

        gsr_capture_v4l2_supported_pixfmts supported_pixfmts = gsr_capture_v4l2_get_supported_pixfmts(fd);
        if(!has_libturbojpeg_lib)
            supported_pixfmts.mjpeg = false;

        if(supported_pixfmts.yuyv || supported_pixfmts.mjpeg)
            callback(v4l2_device_path, supported_pixfmts, (vec2i){ fmt.fmt.pix.width, fmt.fmt.pix.height }, userdata);

        next:
        close(fd);
    }
}
