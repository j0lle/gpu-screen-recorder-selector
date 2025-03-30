#include "../include/utils.h"
#include "../include/window/window.h"

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <errno.h>
#include <assert.h>

#include <xf86drmMode.h>
#include <xf86drm.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vaapi.h>

double clock_get_monotonic_seconds(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

bool generate_random_characters(char *buffer, int buffer_size, const char *alphabet, size_t alphabet_size) {
    /* TODO: Use other functions on other platforms than linux */
    if(getrandom(buffer, buffer_size, 0) < buffer_size) {
        fprintf(stderr, "Failed to get random bytes, error: %s\n", strerror(errno));
        return false;
    }

    for(int i = 0; i < buffer_size; ++i) {
        unsigned char c = *(unsigned char*)&buffer[i];
        buffer[i] = alphabet[c % alphabet_size];
    }

    return true;
}

bool generate_random_characters_standard_alphabet(char *buffer, int buffer_size) {
    return generate_random_characters(buffer, buffer_size, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62);
}

static const XRRModeInfo* get_mode_info(const XRRScreenResources *sr, RRMode id) {
    for(int i = 0; i < sr->nmode; ++i) {
        if(sr->modes[i].id == id)
            return &sr->modes[i];
    }    
    return NULL;
}

static gsr_monitor_rotation x11_rotation_to_gsr_rotation(int rot) {
    switch(rot) {
        case RR_Rotate_0:   return GSR_MONITOR_ROT_0;
        case RR_Rotate_90:  return GSR_MONITOR_ROT_90;
        case RR_Rotate_180: return GSR_MONITOR_ROT_180;
        case RR_Rotate_270: return GSR_MONITOR_ROT_270;
    }
    return GSR_MONITOR_ROT_0;
}

static uint32_t x11_output_get_connector_id(Display *dpy, RROutput output, Atom randr_connector_id_atom) {
    Atom type = 0;
    int format = 0;
    unsigned long bytes_after = 0;
    unsigned long nitems = 0;
    unsigned char *prop = NULL;
    XRRGetOutputProperty(dpy, output, randr_connector_id_atom, 0, 128, false, false, AnyPropertyType, &type, &format, &nitems, &bytes_after, &prop);

    long result = 0;
    if(type == XA_INTEGER && format == 32)
        result = *(long*)prop;

    free(prop);
    return result;
}

static vec2i get_monitor_size_rotated(int width, int height, gsr_monitor_rotation rotation) {
    vec2i size = { .x = width, .y = height };
    if(rotation == GSR_MONITOR_ROT_90 || rotation == GSR_MONITOR_ROT_270) {
        int tmp_x = size.x;
        size.x = size.y;
        size.y = tmp_x;
    }
    return size;
}

void for_each_active_monitor_output_x11_not_cached(Display *display, active_monitor_callback callback, void *userdata) {
    XRRScreenResources *screen_res = XRRGetScreenResources(display, DefaultRootWindow(display));
    if(!screen_res)
        return;

    const Atom randr_connector_id_atom = XInternAtom(display, "CONNECTOR_ID", False);

    char display_name[256];
    for(int i = 0; i < screen_res->noutput; ++i) {
        XRROutputInfo *out_info = XRRGetOutputInfo(display, screen_res, screen_res->outputs[i]);
        if(out_info && out_info->crtc && out_info->connection == RR_Connected) {
            XRRCrtcInfo *crt_info = XRRGetCrtcInfo(display, screen_res, out_info->crtc);
            if(crt_info && crt_info->mode) {
                // We want to use the current mode info width/height (mode_info->width/height) instead of crtc info width/height (crt_info->width/height) because crtc info
                // is scaled if the monitor is scaled (xrandr --output DP-1 --scale 1.5). Normally this is not an issue for x11 applications,
                // but gpu screen recorder captures the drm framebuffer instead of x11 api. This drm framebuffer which doesn't increase in size when using xrandr scaling.
                // Maybe a better option would be to get the drm crtc size instead.
                const XRRModeInfo *mode_info = get_mode_info(screen_res, crt_info->mode);
                if(mode_info && out_info->nameLen < (int)sizeof(display_name)) {
                    snprintf(display_name, sizeof(display_name), "%.*s", (int)out_info->nameLen, out_info->name);
                    const gsr_monitor_rotation rotation = x11_rotation_to_gsr_rotation(crt_info->rotation);
                    const vec2i monitor_size = get_monitor_size_rotated(mode_info->width, mode_info->height, rotation);

                    const gsr_monitor monitor = {
                        .name = display_name,
                        .name_len = out_info->nameLen,
                        .pos = { .x = crt_info->x, .y = crt_info->y },
                        .size = monitor_size,
                        .connector_id = x11_output_get_connector_id(display, screen_res->outputs[i], randr_connector_id_atom),
                        .rotation = rotation,
                        .monitor_identifier = out_info->crtc
                    };
                    callback(&monitor, userdata);
                }
            }
            if(crt_info)
                XRRFreeCrtcInfo(crt_info);
        }
        if(out_info)
            XRRFreeOutputInfo(out_info);
    }    

    XRRFreeScreenResources(screen_res);
}

/* TODO: Support more connector types */
int get_connector_type_by_name(const char *name) {
    int len = strlen(name);
    if(len >= 5 && strncmp(name, "HDMI-", 5) == 0)
        return 1;
    else if(len >= 3 && strncmp(name, "DP-", 3) == 0)
        return 2;
    else if(len >= 12 && strncmp(name, "DisplayPort-", 12) == 0)
        return 3;
    else if(len >= 4 && strncmp(name, "eDP-", 4) == 0)
        return 4;
    else
        return -1;
}

drm_connector_type_count* drm_connector_types_get_index(drm_connector_type_count *type_counts, int *num_type_counts, int connector_type) {
    for(int i = 0; i < *num_type_counts; ++i) {
        if(type_counts[i].type == connector_type)
            return &type_counts[i];
    }

    if(*num_type_counts == CONNECTOR_TYPE_COUNTS)
        return NULL;

    const int index = *num_type_counts;
    type_counts[index].type = connector_type;
    type_counts[index].count = 0;
    type_counts[index].count_active = 0;
    ++*num_type_counts;
    return &type_counts[index];
}

uint32_t monitor_identifier_from_type_and_count(int monitor_type_index, int monitor_type_count) {
    return ((uint32_t)monitor_type_index << 16) | ((uint32_t)monitor_type_count);
}

static bool connector_get_property_by_name(int drmfd, drmModeConnectorPtr props, const char *name, uint64_t *result) {
    for(int i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drmfd, props->props[i]);
        if(prop) {
            if(strcmp(name, prop->name) == 0) {
                *result = props->prop_values[i];
                drmModeFreeProperty(prop);
                return true;
            }
            drmModeFreeProperty(prop);
        }
    }
    return false;
}

static void for_each_active_monitor_output_drm(const char *card_path, active_monitor_callback callback, void *userdata) {
    int fd = open(card_path, O_RDONLY);
    if(fd == -1) {
        fprintf(stderr, "gsr error: for_each_active_monitor_output_drm failed, failed to open \"%s\", error: %s\n", card_path, strerror(errno));
        return;
    }

    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drm_connector_type_count type_counts[CONNECTOR_TYPE_COUNTS];
    int num_type_counts = 0;

    char display_name[256];
    drmModeResPtr resources = drmModeGetResources(fd);
    if(resources) {
        for(int i = 0; i < resources->count_connectors; ++i) {
            drmModeConnectorPtr connector = drmModeGetConnectorCurrent(fd, resources->connectors[i]);
            if(!connector)
                continue;

            drm_connector_type_count *connector_type = drm_connector_types_get_index(type_counts, &num_type_counts, connector->connector_type);
            const char *connection_name = drmModeGetConnectorTypeName(connector->connector_type);
            const int connection_name_len = strlen(connection_name);
            if(connector_type)
                ++connector_type->count;

            if(connector->connection != DRM_MODE_CONNECTED) {
                drmModeFreeConnector(connector);
                continue;
            }

            if(connector_type)
                ++connector_type->count_active;

            uint64_t crtc_id = 0;
            connector_get_property_by_name(fd, connector, "CRTC_ID", &crtc_id);

            drmModeCrtcPtr crtc = drmModeGetCrtc(fd, crtc_id);
            if(connector_type && crtc_id > 0 && crtc && connection_name_len + 5 < (int)sizeof(display_name)) {
                const int display_name_len = snprintf(display_name, sizeof(display_name), "%s-%d", connection_name, connector_type->count);
                const int connector_type_index_name = get_connector_type_by_name(display_name);
                gsr_monitor monitor = {
                    .name = display_name,
                    .name_len = display_name_len,
                    .pos = { .x = crtc->x, .y = crtc->y },
                    .size = { .x = (int)crtc->width, .y = (int)crtc->height },
                    .connector_id = connector->connector_id,
                    .rotation = GSR_MONITOR_ROT_0,
                    .monitor_identifier = connector_type_index_name != -1 ? monitor_identifier_from_type_and_count(connector_type_index_name, connector_type->count_active) : 0
                };
                callback(&monitor, userdata);
            }

            if(crtc)
                drmModeFreeCrtc(crtc);

            drmModeFreeConnector(connector);
        }
        drmModeFreeResources(resources);
    }

    close(fd);
}

void for_each_active_monitor_output(const gsr_window *window, const char *card_path, gsr_connection_type connection_type, active_monitor_callback callback, void *userdata) {
    switch(connection_type) {
        case GSR_CONNECTION_X11:
        case GSR_CONNECTION_WAYLAND:
            gsr_window_for_each_active_monitor_output_cached(window, callback, userdata);
            break;
        case GSR_CONNECTION_DRM:
            for_each_active_monitor_output_drm(card_path, callback, userdata);
            break;
    }
}

static void get_monitor_by_name_callback(const gsr_monitor *monitor, void *userdata) {
    get_monitor_by_name_userdata *data = (get_monitor_by_name_userdata*)userdata;
    if(!data->found_monitor && strcmp(data->name, monitor->name) == 0) {
        data->monitor->pos = monitor->pos;
        data->monitor->size = monitor->size;
        data->monitor->connector_id = monitor->connector_id;
        data->monitor->rotation = monitor->rotation;
        data->monitor->monitor_identifier = monitor->monitor_identifier;
        data->found_monitor = true;
    }
}

bool get_monitor_by_name(const gsr_egl *egl, gsr_connection_type connection_type, const char *name, gsr_monitor *monitor) {
    get_monitor_by_name_userdata userdata;
    userdata.name = name;
    userdata.name_len = strlen(name);
    userdata.monitor = monitor;
    userdata.found_monitor = false;
    for_each_active_monitor_output(egl->window, egl->card_path, connection_type, get_monitor_by_name_callback, &userdata);
    return userdata.found_monitor;
}

typedef struct {
    const gsr_monitor *monitor;
    gsr_monitor_rotation rotation;
    vec2i position;
    bool match_found;
} get_monitor_by_connector_id_userdata;

static bool vec2i_eql(vec2i a, vec2i b) {
    return a.x == b.x && a.y == b.y;
}

static void get_monitor_by_name_and_size_callback(const gsr_monitor *monitor, void *userdata) {
    get_monitor_by_connector_id_userdata *data = (get_monitor_by_connector_id_userdata*)userdata;
    if(monitor->name && data->monitor->name && strcmp(monitor->name, data->monitor->name) == 0 && vec2i_eql(monitor->size, data->monitor->size)) {
        data->rotation = monitor->rotation;
        data->position = monitor->pos;
        data->match_found = true;
    }
}

static void get_monitor_by_connector_id_callback(const gsr_monitor *monitor, void *userdata) {
    get_monitor_by_connector_id_userdata *data = (get_monitor_by_connector_id_userdata*)userdata;
    if(monitor->connector_id == data->monitor->connector_id ||
        (!monitor->connector_id && monitor->monitor_identifier == data->monitor->monitor_identifier))
    {
        data->rotation = monitor->rotation;
        data->position = monitor->pos;
        data->match_found = true;
    }
}

bool drm_monitor_get_display_server_data(const gsr_window *window, const gsr_monitor *monitor, gsr_monitor_rotation *monitor_rotation, vec2i *monitor_position) {
    *monitor_rotation = GSR_MONITOR_ROT_0;
    *monitor_position = (vec2i){0, 0};

    if(gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_WAYLAND) {
        {
            get_monitor_by_connector_id_userdata userdata;
            userdata.monitor = monitor;
            userdata.rotation = GSR_MONITOR_ROT_0;
            userdata.position = (vec2i){0, 0};
            userdata.match_found = false;
            gsr_window_for_each_active_monitor_output_cached(window, get_monitor_by_name_and_size_callback, &userdata);
            if(userdata.match_found) {
                *monitor_rotation = userdata.rotation;
                *monitor_position = userdata.position;
                return true;
            }
        }
        {
            get_monitor_by_connector_id_userdata userdata;
            userdata.monitor = monitor;
            userdata.rotation = GSR_MONITOR_ROT_0;
            userdata.position = (vec2i){0, 0};
            userdata.match_found = false;
            gsr_window_for_each_active_monitor_output_cached(window, get_monitor_by_connector_id_callback, &userdata);
            *monitor_rotation = userdata.rotation;
            *monitor_position = userdata.position;
            return userdata.match_found;
        }
    } else {
        get_monitor_by_connector_id_userdata userdata;
        userdata.monitor = monitor;
        userdata.rotation = GSR_MONITOR_ROT_0;
        userdata.position = (vec2i){0, 0};
        userdata.match_found = false;
        gsr_window_for_each_active_monitor_output_cached(window, get_monitor_by_connector_id_callback, &userdata);
        *monitor_rotation = userdata.rotation;
        *monitor_position = userdata.position;
        return userdata.match_found;
    }
}

bool gl_get_gpu_info(gsr_egl *egl, gsr_gpu_info *info) {
    const char *software_renderers[] = { "llvmpipe", "SWR", "softpipe", NULL };
    bool supported = true;
    const unsigned char *gl_vendor = egl->glGetString(GL_VENDOR);
    const unsigned char *gl_renderer = egl->glGetString(GL_RENDERER);
    const unsigned char *gl_version = egl->glGetString(GL_VERSION);

    info->gpu_version = 0;
    info->is_steam_deck = false;
    info->driver_major = 0;
    info->driver_minor = 0;
    info->driver_patch = 0;

    if(!gl_vendor) {
        fprintf(stderr, "gsr error: failed to get gpu vendor\n");
        supported = false;
        goto end;
    }

    if(gl_renderer) {
        for(int i = 0; software_renderers[i]; ++i) {
            if(strstr((const char*)gl_renderer, software_renderers[i])) {
                fprintf(stderr, "gsr error: your opengl environment is not properly setup. It's using %s (software rendering) for opengl instead of your graphics card. Please make sure your graphics driver is properly installed\n", software_renderers[i]);
                supported = false;
                goto end;
            }
        }
    }

    if(strstr((const char*)gl_vendor, "AMD"))
        info->vendor = GSR_GPU_VENDOR_AMD;
    else if(strstr((const char*)gl_vendor, "Mesa") && gl_renderer && strstr((const char*)gl_renderer, "AMD"))
        info->vendor = GSR_GPU_VENDOR_AMD;
    else if(strstr((const char*)gl_vendor, "Intel"))
        info->vendor = GSR_GPU_VENDOR_INTEL;
    else if(strstr((const char*)gl_vendor, "NVIDIA"))
        info->vendor = GSR_GPU_VENDOR_NVIDIA;
    else if(strstr((const char*)gl_vendor, "Broadcom"))
        info->vendor = GSR_GPU_VENDOR_BROADCOM;
    else {
        fprintf(stderr, "gsr error: unknown gpu vendor: %s\n", gl_vendor);
        supported = false;
        goto end;
    }

    if(gl_renderer) {
        if(info->vendor == GSR_GPU_VENDOR_NVIDIA)
            sscanf((const char*)gl_renderer, "%*s %*s %*s %d", &info->gpu_version);
        info->is_steam_deck = strstr((const char*)gl_renderer, "vangogh") != NULL;
    }

    if(gl_version) {
        const char *mesa_p = strstr((const char*)gl_version, "Mesa ");
        if(mesa_p) {
            mesa_p += 5;
            int major = 0;
            int minor = 0;
            int patch = 0;
            if(sscanf(mesa_p, "%d.%d.%d", &major, &minor, &patch) == 3) {
                info->driver_major = major;
                info->driver_minor = minor;
                info->driver_patch = patch;
            }
        }
    }

    end:
    return supported;
}

bool version_greater_than(int major, int minor, int patch, int other_major, int other_minor, int other_patch) {
    return (major > other_major) || (major == other_major && minor > other_minor) || (major == other_major && minor == other_minor && patch > other_patch);
}

bool gl_driver_version_greater_than(const gsr_gpu_info *gpu_info, int major, int minor, int patch) {
    return version_greater_than(gpu_info->driver_major, gpu_info->driver_minor, gpu_info->driver_patch, major, minor, patch);
}

bool try_card_has_valid_plane(const char *card_path) {
    drmVersion *ver = NULL;
    drmModePlaneResPtr planes = NULL;
    bool found_screen_card = false;

    int fd = open(card_path, O_RDONLY);
    if(fd == -1)
        return false;

    ver = drmGetVersion(fd);
    if(!ver || strstr(ver->name, "nouveau"))
        goto next;

    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    planes = drmModeGetPlaneResources(fd);
    if(!planes)
        goto next;

    for(uint32_t j = 0; j < planes->count_planes; ++j) {
        drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[j]);
        if(!plane)
            continue;

        if(plane->fb_id)
            found_screen_card = true;

        drmModeFreePlane(plane);
        if(found_screen_card)
            break;
    }

    next:
    if(planes)
        drmModeFreePlaneResources(planes);
    if(ver)
        drmFreeVersion(ver);
    close(fd);
    if(found_screen_card)
        return true;

    return false;
}

bool gsr_get_valid_card_path(gsr_egl *egl, char *output, bool is_monitor_capture) {
    if(egl->dri_card_path) {
        snprintf(output, 128, "%s", egl->dri_card_path);
        return is_monitor_capture ? try_card_has_valid_plane(output) : true;
    }

    for(int i = 0; i < 10; ++i) {
        snprintf(output, 128, DRM_DEV_NAME, DRM_DIR_NAME, i);
        if(try_card_has_valid_plane(output))
            return true;
    }
    return false;
}

bool gsr_card_path_get_render_path(const char *card_path, char *render_path) {
    int fd = open(card_path, O_RDONLY);
    if(fd == -1)
        return false;

    char *render_path_tmp = drmGetRenderDeviceNameFromFd(fd);
    if(render_path_tmp) {
        snprintf(render_path, 128, "%s", render_path_tmp);
        free(render_path_tmp);
        close(fd);
        return true;
    }

    close(fd);
    return false;
}

int create_directory_recursive(char *path) {
    int path_len = strlen(path);
    char *p = path;
    char *end = path + path_len;
    for(;;) {
        char *slash_p = strchr(p, '/');

        // Skips first '/', we don't want to try and create the root directory
        if(slash_p == path) {
            ++p;
            continue;
        }

        if(!slash_p)
            slash_p = end;

        char prev_char = *slash_p;
        *slash_p = '\0';
        int err = mkdir(path, S_IRWXU);
        *slash_p = prev_char;

        if(err == -1 && errno != EEXIST)
            return err;

        if(slash_p == end)
            break;
        else
            p = slash_p + 1;
    }
    return 0;
}

void setup_dma_buf_attrs(intptr_t *img_attr, uint32_t format, uint32_t width, uint32_t height, const int *fds, const uint32_t *offsets, const uint32_t *pitches, const uint64_t *modifiers, int num_planes, bool use_modifier) {
    size_t img_attr_index = 0;

    img_attr[img_attr_index++] = EGL_LINUX_DRM_FOURCC_EXT;
    img_attr[img_attr_index++] = format;

    img_attr[img_attr_index++] = EGL_WIDTH;
    img_attr[img_attr_index++] = width;

    img_attr[img_attr_index++] = EGL_HEIGHT;
    img_attr[img_attr_index++] = height;

    if(num_planes >= 1) {
        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        img_attr[img_attr_index++] = fds[0];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        img_attr[img_attr_index++] = offsets[0];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        img_attr[img_attr_index++] = pitches[0];

        if(use_modifier) {
            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            img_attr[img_attr_index++] = modifiers[0] & 0xFFFFFFFFULL;

            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            img_attr[img_attr_index++] = modifiers[0] >> 32ULL;
        }
    }

    if(num_planes >= 2) {
        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_FD_EXT;
        img_attr[img_attr_index++] = fds[1];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
        img_attr[img_attr_index++] = offsets[1];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
        img_attr[img_attr_index++] = pitches[1];

        if(use_modifier) {
            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
            img_attr[img_attr_index++] = modifiers[1] & 0xFFFFFFFFULL;

            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
            img_attr[img_attr_index++] = modifiers[1] >> 32ULL;
        }
    }

    if(num_planes >= 3) {
        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_FD_EXT;
        img_attr[img_attr_index++] = fds[2];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
        img_attr[img_attr_index++] = offsets[2];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
        img_attr[img_attr_index++] = pitches[2];

        if(use_modifier) {
            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
            img_attr[img_attr_index++] = modifiers[2] & 0xFFFFFFFFULL;

            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
            img_attr[img_attr_index++] = modifiers[2] >> 32ULL;
        }
    }

    if(num_planes >= 4) {
        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_FD_EXT;
        img_attr[img_attr_index++] = fds[3];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
        img_attr[img_attr_index++] = offsets[3];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
        img_attr[img_attr_index++] = pitches[3];

        if(use_modifier) {
            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
            img_attr[img_attr_index++] = modifiers[3] & 0xFFFFFFFFULL;

            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
            img_attr[img_attr_index++] = modifiers[3] >> 32ULL;
        }
    }

    img_attr[img_attr_index++] = EGL_NONE;
    assert(img_attr_index <= 44);
}

static VADisplay video_codec_context_get_vaapi_display(AVCodecContext *video_codec_context) {
    AVBufferRef *hw_frames_ctx = video_codec_context->hw_frames_ctx;
    if(!hw_frames_ctx)
        return NULL;

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)hw_frames_ctx->data;
    AVHWDeviceContext *device_context = (AVHWDeviceContext*)hw_frame_context->device_ctx;
    if(device_context->type != AV_HWDEVICE_TYPE_VAAPI)
        return NULL;

    AVVAAPIDeviceContext *vactx = device_context->hwctx;
    return vactx->display;
}

bool video_codec_context_is_vaapi(AVCodecContext *video_codec_context) {
    if(!video_codec_context)
        return false;

    AVBufferRef *hw_frames_ctx = video_codec_context->hw_frames_ctx;
    if(!hw_frames_ctx)
        return false;

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)hw_frames_ctx->data;
    AVHWDeviceContext *device_context = (AVHWDeviceContext*)hw_frame_context->device_ctx;
    return device_context->type == AV_HWDEVICE_TYPE_VAAPI;
}

vec2i scale_keep_aspect_ratio(vec2i from, vec2i to) {
    if(from.x == 0 || from.y == 0)
        return (vec2i){0, 0};

    const double height_to_width_ratio = (double)from.y / (double)from.x;
    from.x = to.x;
    from.y = from.x * height_to_width_ratio;
    
    if(from.y > to.y) {
        const double width_height_ratio = (double)from.x / (double)from.y;
        from.y = to.y;
        from.x = from.y * width_height_ratio;
    }

    return from;
}

unsigned int gl_create_texture(gsr_egl *egl, int width, int height, int internal_format, unsigned int format, int filter) {
    unsigned int texture_id = 0;
    egl->glGenTextures(1, &texture_id);
    egl->glBindTexture(GL_TEXTURE_2D, texture_id);
    egl->glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, NULL);

    const float border_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    egl->glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);

    egl->glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}
