#ifndef GSR_COLOR_CONVERSION_H
#define GSR_COLOR_CONVERSION_H

#include "shader.h"
#include "defs.h"
#include "vec2.h"
#include <stdbool.h>

#define GSR_COLOR_CONVERSION_MAX_GRAPHICS_SHADERS 12
#define GSR_COLOR_CONVERSION_MAX_FRAMEBUFFERS 2

typedef enum {
    GSR_SOURCE_COLOR_RGB,
    GSR_SOURCE_COLOR_BGR,
    GSR_SOURCE_COLOR_YUYV
} gsr_source_color;

typedef enum {
    GSR_DESTINATION_COLOR_NV12, /* YUV420, BT709, 8-bit */
    GSR_DESTINATION_COLOR_P010, /* YUV420, BT2020, 10-bit */
    GSR_DESTINATION_COLOR_RGB8
} gsr_destination_color;

typedef enum {
    GSR_ROT_0,
    GSR_ROT_90,
    GSR_ROT_180,
    GSR_ROT_270
} gsr_rotation;

typedef enum {
    GSR_FLIP_NONE       = 0,
    GSR_FLIP_HORIZONTAL = (1 << 0),
    GSR_FLIP_VERTICAL   = (1 << 1)
} gsr_flip;

typedef struct {
    int rotation_matrix;
    int offset;
} gsr_color_graphics_uniforms;

typedef struct {
    gsr_egl *egl;

    gsr_destination_color destination_color;

    unsigned int destination_textures[2];
    vec2i destination_textures_size[2];
    int num_destination_textures;

    gsr_color_range color_range;
    bool load_external_image_shader;
} gsr_color_conversion_params;

typedef struct {
    gsr_color_conversion_params params;

    gsr_color_graphics_uniforms graphics_uniforms[GSR_COLOR_CONVERSION_MAX_GRAPHICS_SHADERS];
    gsr_shader graphics_shaders[GSR_COLOR_CONVERSION_MAX_GRAPHICS_SHADERS];

    unsigned int framebuffers[GSR_COLOR_CONVERSION_MAX_FRAMEBUFFERS];

    unsigned int vertex_array_object_id;
    unsigned int vertex_buffer_object_id;

    bool schedule_clear;
} gsr_color_conversion;

int gsr_color_conversion_init(gsr_color_conversion *self, const gsr_color_conversion_params *params);
void gsr_color_conversion_deinit(gsr_color_conversion *self);

void gsr_color_conversion_draw(gsr_color_conversion *self, unsigned int texture_id, vec2i destination_pos, vec2i destination_size, vec2i source_pos, vec2i source_size, vec2i texture_size, gsr_rotation rotation, gsr_flip flip, gsr_source_color source_color, bool external_texture);
void gsr_color_conversion_clear(gsr_color_conversion *self);
void gsr_color_conversion_read_destination_texture(gsr_color_conversion *self, int destination_texture_index, int x, int y, int width, int height, unsigned int color_format, unsigned int data_format, void *pixels);

gsr_rotation gsr_monitor_rotation_to_rotation(gsr_monitor_rotation monitor_rotation);

#endif /* GSR_COLOR_CONVERSION_H */
