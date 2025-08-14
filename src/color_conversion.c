#include "../include/color_conversion.h"
#include "../include/egl.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>

#define COMPUTE_SHADER_INDEX_Y                  0
#define COMPUTE_SHADER_INDEX_UV                 1
#define COMPUTE_SHADER_INDEX_Y_EXTERNAL         2
#define COMPUTE_SHADER_INDEX_UV_EXTERNAL        3
#define COMPUTE_SHADER_INDEX_RGB                4
#define COMPUTE_SHADER_INDEX_RGB_EXTERNAL       5
#define COMPUTE_SHADER_INDEX_Y_BLEND            6
#define COMPUTE_SHADER_INDEX_UV_BLEND           7
#define COMPUTE_SHADER_INDEX_Y_EXTERNAL_BLEND   8
#define COMPUTE_SHADER_INDEX_UV_EXTERNAL_BLEND  9
#define COMPUTE_SHADER_INDEX_RGB_BLEND          10
#define COMPUTE_SHADER_INDEX_RGB_EXTERNAL_BLEND 11

#define GRAPHICS_SHADER_INDEX_Y                  0
#define GRAPHICS_SHADER_INDEX_UV                 1
#define GRAPHICS_SHADER_INDEX_Y_EXTERNAL         2
#define GRAPHICS_SHADER_INDEX_UV_EXTERNAL        3
#define GRAPHICS_SHADER_INDEX_RGB                4
#define GRAPHICS_SHADER_INDEX_RGB_EXTERNAL       5

/* https://en.wikipedia.org/wiki/YCbCr, see study/color_space_transform_matrix.png */

/* ITU-R BT2020, full */
/* https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2020-2-201510-I!!PDF-E.pdf */
#define RGB_TO_P010_FULL "const mat4 RGBtoYUV = mat4(0.262700, -0.139630,  0.500000, 0.000000,\n" \
                         "                           0.678000, -0.360370, -0.459786, 0.000000,\n" \
                         "                           0.059300,  0.500000, -0.040214, 0.000000,\n" \
                         "                           0.000000,  0.500000,  0.500000, 1.000000);\n"

/* ITU-R BT2020, limited (full multiplied by (235-16)/255, adding 16/255 to luma) */
#define RGB_TO_P010_LIMITED "const mat4 RGBtoYUV = mat4(0.225613, -0.119918,  0.429412, 0.000000,\n" \
                            "                           0.582282, -0.309494, -0.394875, 0.000000,\n" \
                            "                           0.050928,  0.429412, -0.034537, 0.000000,\n" \
                            "                           0.062745,  0.500000,  0.500000, 1.000000);\n"

/* ITU-R BT709, full, custom values: 0.2110 0.7110 0.0710 */
/* https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf */
#define RGB_TO_NV12_FULL "const mat4 RGBtoYUV = mat4(0.211000, -0.113563,  0.500000, 0.000000,\n" \
                         "                           0.711000, -0.382670, -0.450570, 0.000000,\n" \
                         "                           0.071000,  0.500000, -0.044994, 0.000000,\n" \
                         "                           0.000000,  0.500000,  0.500000, 1.000000);\n"

/* ITU-R BT709, limited, custom values: 0.2100 0.7100 0.0700 (full multiplied by (235-16)/255, adding 16/255 to luma) */
#define RGB_TO_NV12_LIMITED "const mat4 RGBtoYUV = mat4(0.180353, -0.096964,  0.429412, 0.000000,\n" \
                            "                           0.609765, -0.327830, -0.385927, 0.000000,\n" \
                            "                           0.060118,  0.429412, -0.038049, 0.000000,\n" \
                            "                           0.062745,  0.500000,  0.500000, 1.000000);\n"

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static const char* color_format_range_get_transform_matrix(gsr_destination_color color_format, gsr_color_range color_range) {
    switch(color_format) {
        case GSR_DESTINATION_COLOR_NV12: {
            switch(color_range) {
                case GSR_COLOR_RANGE_LIMITED:
                    return RGB_TO_NV12_LIMITED;
                case GSR_COLOR_RANGE_FULL:
                    return RGB_TO_NV12_FULL;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_P010: {
            switch(color_range) {
                case GSR_COLOR_RANGE_LIMITED:
                    return RGB_TO_P010_LIMITED;
                case GSR_COLOR_RANGE_FULL:
                    return RGB_TO_P010_FULL;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8:
            return "";
        default:
            return NULL;
    }
    return NULL;
}

static void get_compute_shader_header(char *header, size_t header_size, bool external_texture) {
    if(external_texture) {
        snprintf(header, header_size,
            "#version 310 es\n"
            "#extension GL_OES_EGL_image_external : enable\n"
            "#extension GL_OES_EGL_image_external_essl3 : require\n"
            "layout(binding = 0) uniform highp samplerExternalOES img_input;\n"
            "layout(binding = 1) uniform highp sampler2D img_background;\n");
    } else {
        snprintf(header, header_size,
            "#version 310 es\n"
            "layout(binding = 0) uniform highp sampler2D img_input;\n"
            "layout(binding = 1) uniform highp sampler2D img_background;\n");
    }
}

static int load_compute_shader_y(gsr_shader *shader, gsr_egl *egl, gsr_color_compute_uniforms *uniforms, int max_local_size_dim, gsr_destination_color color_format, gsr_color_range color_range, bool external_texture, bool alpha_blending) {
    const char *color_transform_matrix = color_format_range_get_transform_matrix(color_format, color_range);

    char header[512];
    get_compute_shader_header(header, sizeof(header), external_texture);

    char compute_shader[4096];
    snprintf(compute_shader, sizeof(compute_shader),
        "%s"
        "layout (local_size_x = %d, local_size_y = %d, local_size_z = 1) in;\n"
        "precision highp float;\n"
        "uniform ivec2 source_position;\n"
        "uniform ivec2 target_position;\n"
        "uniform vec2 scale;\n"
        "uniform mat2 rotation_matrix;\n"
        "layout(rgba8, binding = 0) writeonly uniform highp image2D img_output;\n"
        "%s"
        "void main() {\n"
        "    ivec2 texel_coord = ivec2(gl_GlobalInvocationID.xy);\n"
        "    ivec2 size = ivec2(vec2(textureSize(img_input, 0)) * scale + 0.5);\n"
        "    ivec2 size_shift = size >> 1;\n" // size/2
        "    ivec2 output_size = textureSize(img_background, 0);\n"
        "    vec2 rotated_texel_coord = vec2(texel_coord - source_position - size_shift) * rotation_matrix + vec2(size_shift) + 0.5;\n"
        "    vec2 output_texel_coord = vec2(texel_coord - source_position + target_position) + 0.5;\n"
        "    vec2 source_color_coords = rotated_texel_coord/vec2(size);\n"
        "    vec4 source_color = texture(img_input, source_color_coords);\n"
        "    if(source_color_coords.x > 1.0 || source_color_coords.y > 1.0)\n"
        "        source_color.rgba = vec4(0.0, 0.0, 0.0, %s);\n"
        "    vec4 source_color_yuv = RGBtoYUV * vec4(source_color.rgb, 1.0);\n"
        "    vec4 output_color_yuv = %s;\n"
        "    float y_color = mix(output_color_yuv.r, source_color_yuv.r, source_color.a);\n"
        "    imageStore(img_output, texel_coord + target_position, vec4(y_color, 1.0, 1.0, 1.0));\n"
        "}\n", header, max_local_size_dim, max_local_size_dim, color_transform_matrix,
            alpha_blending ? "0.0" : "1.0",
            alpha_blending ? "texture(img_background, output_texel_coord/vec2(output_size))" : "source_color_yuv");

    if(gsr_shader_init(shader, egl, NULL, NULL, compute_shader) != 0)
        return -1;

    uniforms->source_position = egl->glGetUniformLocation(shader->program_id, "source_position");
    uniforms->target_position = egl->glGetUniformLocation(shader->program_id, "target_position");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    uniforms->scale = egl->glGetUniformLocation(shader->program_id, "scale");
    return 0;
}

static int load_compute_shader_uv(gsr_shader *shader, gsr_egl *egl, gsr_color_compute_uniforms *uniforms, int max_local_size_dim, gsr_destination_color color_format, gsr_color_range color_range, bool external_texture, bool alpha_blending) {
    const char *color_transform_matrix = color_format_range_get_transform_matrix(color_format, color_range);

    char header[512];
    get_compute_shader_header(header, sizeof(header), external_texture);

    char compute_shader[4096];
    snprintf(compute_shader, sizeof(compute_shader),
        "%s"
        "layout (local_size_x = %d, local_size_y = %d, local_size_z = 1) in;\n"
        "precision highp float;\n"
        "uniform ivec2 source_position;\n"
        "uniform ivec2 target_position;\n"
        "uniform vec2 scale;\n"
        "uniform mat2 rotation_matrix;\n"
        "layout(rgba8, binding = 0) writeonly uniform highp image2D img_output;\n"
        "%s"
        "void main() {\n"
        "    ivec2 texel_coord = ivec2(gl_GlobalInvocationID.xy);\n"
        "    ivec2 size = ivec2(vec2(textureSize(img_input, 0)) * scale + 0.5);\n"
        "    ivec2 size_shift = size >> 2;\n" // size/4
        "    ivec2 output_size = textureSize(img_background, 0);\n"
        "    vec2 rotated_texel_coord = vec2(texel_coord - source_position - size_shift) * rotation_matrix + vec2(size_shift) + 0.5;\n"
        "    vec2 output_texel_coord = vec2(texel_coord - source_position + target_position) + 0.5;\n"
        "    vec2 source_color_coords = rotated_texel_coord/vec2(size>>1);\n"
        "    vec4 source_color = texture(img_input, source_color_coords);\n" // size/2
        "    if(source_color_coords.x > 1.0 || source_color_coords.y > 1.0)\n"
        "        source_color.rgba = vec4(0.0, 0.0, 0.0, %s);\n"
        "    vec4 source_color_yuv = RGBtoYUV * vec4(source_color.rgb, 1.0);\n"
        "    vec4 output_color_yuv = %s;\n"
        "    vec2 uv_color = mix(output_color_yuv.rg, source_color_yuv.gb, source_color.a);\n"
        "    imageStore(img_output, texel_coord + target_position, vec4(uv_color, 1.0, 1.0));\n"
        "}\n", header, max_local_size_dim, max_local_size_dim, color_transform_matrix,
            alpha_blending ? "0.0" : "1.0",
            alpha_blending ? "texture(img_background, output_texel_coord/vec2(output_size))" : "source_color_yuv");

    if(gsr_shader_init(shader, egl, NULL, NULL, compute_shader) != 0)
        return -1;

    uniforms->source_position = egl->glGetUniformLocation(shader->program_id, "source_position");
    uniforms->target_position = egl->glGetUniformLocation(shader->program_id, "target_position");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    uniforms->scale = egl->glGetUniformLocation(shader->program_id, "scale");
    return 0;
}

static int load_compute_shader_rgb(gsr_shader *shader, gsr_egl *egl, gsr_color_compute_uniforms *uniforms, int max_local_size_dim, bool external_texture, bool alpha_blending) {
    char header[512];
    get_compute_shader_header(header, sizeof(header), external_texture);

    char compute_shader[4096];
    snprintf(compute_shader, sizeof(compute_shader),
        "%s"
        "layout (local_size_x = %d, local_size_y = %d, local_size_z = 1) in;\n"
        "precision highp float;\n"
        "uniform ivec2 source_position;\n"
        "uniform ivec2 target_position;\n"
        "uniform vec2 scale;\n"
        "uniform mat2 rotation_matrix;\n"
        "layout(rgba8, binding = 0) writeonly uniform highp image2D img_output;\n"
        "void main() {\n"
        "    ivec2 texel_coord = ivec2(gl_GlobalInvocationID.xy);\n"
        "    ivec2 size = ivec2(vec2(textureSize(img_input, 0)) * scale + 0.5);\n"
        "    ivec2 size_shift = size >> 1;\n" // size/2
        "    ivec2 output_size = textureSize(img_background, 0);\n"
        "    vec2 rotated_texel_coord = vec2(texel_coord - source_position - size_shift) * rotation_matrix + vec2(size_shift) + 0.5;\n"
        "    vec2 output_texel_coord = vec2(texel_coord - source_position + target_position) + 0.5;\n"
        "    vec2 source_color_coords = rotated_texel_coord/vec2(size);\n"
        "    vec4 source_color = texture(img_input, source_color_coords);\n"
        "    if(source_color_coords.x > 1.0 || source_color_coords.y > 1.0)\n"
        "        source_color.rgba = vec4(0.0, 0.0, 0.0, %s);\n"
        "    vec4 output_color = %s;\n"
        "    vec3 color = mix(output_color.rgb, source_color.rgb, source_color.a);\n"
        "    imageStore(img_output, texel_coord + target_position, vec4(color, 1.0));\n"
        "}\n", header, max_local_size_dim, max_local_size_dim,
            alpha_blending ? "0.0" : "1.0",
            alpha_blending ? "texture(img_background, output_texel_coord/vec2(output_size))" : "source_color");

    if(gsr_shader_init(shader, egl, NULL, NULL, compute_shader) != 0)
        return -1;

    uniforms->source_position = egl->glGetUniformLocation(shader->program_id, "source_position");
    uniforms->target_position = egl->glGetUniformLocation(shader->program_id, "target_position");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    uniforms->scale = egl->glGetUniformLocation(shader->program_id, "scale");
    return 0;
}

static int load_graphics_shader_y(gsr_shader *shader, gsr_egl *egl, gsr_color_graphics_uniforms *uniforms, gsr_destination_color color_format, gsr_color_range color_range, bool external_texture) {
    const char *color_transform_matrix = color_format_range_get_transform_matrix(color_format, color_range);

    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                   \n"
        "in vec2 pos;                                      \n"
        "in vec2 texcoords;                                \n"
        "out vec2 texcoords_out;                           \n"
        "uniform vec2 offset;                              \n"
        "uniform float rotation;                           \n"
        "uniform mat2 rotation_matrix;                     \n"
        "void main()                                       \n"
        "{                                                 \n"
        "  texcoords_out = vec2(texcoords.x - 0.5, texcoords.y - 0.5) * rotation_matrix + vec2(0.5, 0.5);  \n"
        "  gl_Position = vec4(offset.x, offset.y, 0.0, 0.0) + vec4(pos.x, pos.y, 0.0, 1.0);    \n"
        "}                                                 \n");

    const char *main_code =
            "  vec4 pixel = texture(tex1, texcoords_out);                                    \n"
            "  FragColor.x = (RGBtoYUV * vec4(pixel.rgb, 1.0)).x;                            \n"
            "  FragColor.w = pixel.a;                                                        \n";

    char fragment_shader[2048];
    if(external_texture) {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                 \n"
            "#extension GL_OES_EGL_image_external : enable                                   \n"
            "#extension GL_OES_EGL_image_external_essl3 : require                            \n"
            "precision highp float;                                                        \n"
            "in vec2 texcoords_out;                                                          \n"
            "uniform samplerExternalOES tex1;                                                \n"
            "out vec4 FragColor;                                                             \n"
            "%s"
            "void main()                                                                     \n"
            "{                                                                               \n"
            "%s"
            "}                                                                               \n", color_transform_matrix, main_code);
    } else {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                 \n"
            "precision highp float;                                                        \n"
            "in vec2 texcoords_out;                                                          \n"
            "uniform sampler2D tex1;                                                         \n"
            "out vec4 FragColor;                                                             \n"
            "%s"
            "void main()                                                                     \n"
            "{                                                                               \n"
            "%s"
            "}                                                                               \n", color_transform_matrix, main_code);
    }

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader, NULL) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    uniforms->offset = egl->glGetUniformLocation(shader->program_id, "offset");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    return 0;
}

static unsigned int load_graphics_shader_uv(gsr_shader *shader, gsr_egl *egl, gsr_color_graphics_uniforms *uniforms, gsr_destination_color color_format, gsr_color_range color_range, bool external_texture) {
    const char *color_transform_matrix = color_format_range_get_transform_matrix(color_format, color_range);

    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                 \n"
        "in vec2 pos;                                    \n"
        "in vec2 texcoords;                              \n"
        "out vec2 texcoords_out;                         \n"
        "uniform vec2 offset;                            \n"
        "uniform float rotation;                         \n"
        "uniform mat2 rotation_matrix;                   \n"
        "void main()                                     \n"
        "{                                               \n"
        "  texcoords_out = vec2(texcoords.x - 0.5, texcoords.y - 0.5) * rotation_matrix + vec2(0.5, 0.5);                      \n"
        "  gl_Position = (vec4(offset.x, offset.y, 0.0, 0.0) + vec4(pos.x, pos.y, 0.0, 1.0)) * vec4(0.5, 0.5, 1.0, 1.0) - vec4(0.5, 0.5, 0.0, 0.0);   \n"
        "}                                               \n");

    const char *main_code =
            "  vec4 pixel = texture(tex1, texcoords_out);                                          \n"
            "  FragColor.xy = (RGBtoYUV * vec4(pixel.rgb, 1.0)).yz;                                \n"
            "  FragColor.w = pixel.a;                                                              \n";

    char fragment_shader[2048];
    if(external_texture) {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                       \n"
            "#extension GL_OES_EGL_image_external : enable                                         \n"
            "#extension GL_OES_EGL_image_external_essl3 : require                                  \n"
            "precision highp float;                                                              \n"
            "in vec2 texcoords_out;                                                                \n"
            "uniform samplerExternalOES tex1;                                                      \n"
            "out vec4 FragColor;                                                                   \n"
            "%s"
            "void main()                                                                           \n"
            "{                                                                                     \n"
            "%s"
            "}                                                                                     \n", color_transform_matrix, main_code);
    } else {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                       \n"
            "precision highp float;                                                              \n"
            "in vec2 texcoords_out;                                                                \n"
            "uniform sampler2D tex1;                                                               \n"
            "out vec4 FragColor;                                                                   \n"
            "%s"
            "void main()                                                                           \n"
            "{                                                                                     \n"
            "%s"
            "}                                                                                     \n", color_transform_matrix, main_code);
    }

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader, NULL) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    uniforms->offset = egl->glGetUniformLocation(shader->program_id, "offset");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    return 0;
}

static unsigned int load_graphics_shader_rgb(gsr_shader *shader, gsr_egl *egl, gsr_color_graphics_uniforms *uniforms, bool external_texture) {
    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                   \n"
        "in vec2 pos;                                      \n"
        "in vec2 texcoords;                                \n"
        "out vec2 texcoords_out;                           \n"
        "uniform vec2 offset;                              \n"
        "uniform float rotation;                           \n"
        "uniform mat2 rotation_matrix;                     \n"
        "void main()                                       \n"
        "{                                                 \n"
        "  texcoords_out = vec2(texcoords.x - 0.5, texcoords.y - 0.5) * rotation_matrix + vec2(0.5, 0.5);  \n"
        "  gl_Position = vec4(offset.x, offset.y, 0.0, 0.0) + vec4(pos.x, pos.y, 0.0, 1.0);    \n"
        "}                                                 \n");

    const char *main_code =
            "  vec4 pixel = texture(tex1, texcoords_out);                                          \n"
            "  FragColor = pixel;                                                                  \n";

    char fragment_shader[2048];
    if(external_texture) {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                       \n"
            "#extension GL_OES_EGL_image_external : enable                                         \n"
            "#extension GL_OES_EGL_image_external_essl3 : require                                  \n"
            "precision highp float;                                                              \n"
            "in vec2 texcoords_out;                                                                \n"
            "uniform samplerExternalOES tex1;                                                      \n"
            "out vec4 FragColor;                                                                   \n"
            "void main()                                                                           \n"
            "{                                                                                     \n"
            "%s"
            "}                                                                                     \n", main_code);
    } else {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                       \n"
            "precision highp float;                                                              \n"
            "in vec2 texcoords_out;                                                                \n"
            "uniform sampler2D tex1;                                                               \n"
            "out vec4 FragColor;                                                                   \n"
            "void main()                                                                           \n"
            "{                                                                                     \n"
            "%s"
            "}                                                                                     \n", main_code);
    }

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader, NULL) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    uniforms->offset = egl->glGetUniformLocation(shader->program_id, "offset");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    return 0;
}

static int load_framebuffers(gsr_color_conversion *self) {
    /* TODO: Only generate the necessary amount of framebuffers (self->params.num_destination_textures) */
    const unsigned int draw_buffer = GL_COLOR_ATTACHMENT0;
    self->params.egl->glGenFramebuffers(GSR_COLOR_CONVERSION_MAX_FRAMEBUFFERS, self->framebuffers);

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
    self->params.egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->params.destination_textures[0], 0);
    self->params.egl->glDrawBuffers(1, &draw_buffer);
    if(self->params.egl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to create framebuffer for Y\n");
        goto err;
    }

    if(self->params.num_destination_textures > 1) {
        self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
        self->params.egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->params.destination_textures[1], 0);
        self->params.egl->glDrawBuffers(1, &draw_buffer);
        if(self->params.egl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to create framebuffer for UV\n");
            goto err;
        }
    }

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;

    err:
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return -1;
}

static int create_vertices(gsr_color_conversion *self) {
    self->params.egl->glGenVertexArrays(1, &self->vertex_array_object_id);
    self->params.egl->glBindVertexArray(self->vertex_array_object_id);

    self->params.egl->glGenBuffers(1, &self->vertex_buffer_object_id);
    self->params.egl->glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer_object_id);
    self->params.egl->glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), NULL, GL_DYNAMIC_DRAW);

    self->params.egl->glEnableVertexAttribArray(0);
    self->params.egl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    self->params.egl->glEnableVertexAttribArray(1);
    self->params.egl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    self->params.egl->glBindVertexArray(0);
    return 0;
}

static bool gsr_color_conversion_load_compute_shaders(gsr_color_conversion *self) {
    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            if(load_compute_shader_y(&self->compute_shaders[COMPUTE_SHADER_INDEX_Y], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_Y], self->max_local_size_dim, self->params.destination_color, self->params.color_range, false, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                return false;
            }

            if(load_compute_shader_uv(&self->compute_shaders[COMPUTE_SHADER_INDEX_UV], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_UV], self->max_local_size_dim, self->params.destination_color, self->params.color_range, false, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV compute shader\n");
                return false;
            }

            if(load_compute_shader_y(&self->compute_shaders[COMPUTE_SHADER_INDEX_Y_BLEND], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_Y_BLEND], self->max_local_size_dim, self->params.destination_color, self->params.color_range, false, true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                return false;
            }

            if(load_compute_shader_uv(&self->compute_shaders[COMPUTE_SHADER_INDEX_UV_BLEND], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_UV_BLEND], self->max_local_size_dim, self->params.destination_color, self->params.color_range, false, true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV compute shader\n");
                return false;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            if(load_compute_shader_rgb(&self->compute_shaders[COMPUTE_SHADER_INDEX_RGB], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_RGB], self->max_local_size_dim, false, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                return false;
            }

            if(load_compute_shader_rgb(&self->compute_shaders[COMPUTE_SHADER_INDEX_RGB_BLEND], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_RGB_BLEND], self->max_local_size_dim, false, true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                return false;
            }
            break;
        }
    }
    return true;
}

static bool gsr_color_conversion_load_external_compute_shaders(gsr_color_conversion *self) {
    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            if(load_compute_shader_y(&self->compute_shaders[COMPUTE_SHADER_INDEX_Y_EXTERNAL], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_Y_EXTERNAL], self->max_local_size_dim, self->params.destination_color, self->params.color_range, true, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                return false;
            }

            if(load_compute_shader_uv(&self->compute_shaders[COMPUTE_SHADER_INDEX_UV_EXTERNAL], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_UV_EXTERNAL], self->max_local_size_dim, self->params.destination_color, self->params.color_range, true, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV compute shader\n");
                return false;
            }

            if(load_compute_shader_y(&self->compute_shaders[COMPUTE_SHADER_INDEX_Y_EXTERNAL_BLEND], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_Y_EXTERNAL_BLEND], self->max_local_size_dim, self->params.destination_color, self->params.color_range, true, true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                return false;
            }

            if(load_compute_shader_uv(&self->compute_shaders[COMPUTE_SHADER_INDEX_UV_EXTERNAL_BLEND], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_UV_EXTERNAL_BLEND], self->max_local_size_dim, self->params.destination_color, self->params.color_range, true, true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV compute shader\n");
                return false;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            if(load_compute_shader_rgb(&self->compute_shaders[COMPUTE_SHADER_INDEX_RGB_EXTERNAL], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_RGB_EXTERNAL], self->max_local_size_dim, true, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                return false;
            }

            if(load_compute_shader_rgb(&self->compute_shaders[COMPUTE_SHADER_INDEX_RGB_EXTERNAL_BLEND], self->params.egl, &self->compute_uniforms[COMPUTE_SHADER_INDEX_RGB_EXTERNAL_BLEND], self->max_local_size_dim, true, true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                return false;
            }
            break;
        }
    }
    return true;
}

static bool gsr_color_conversion_load_graphics_shaders(gsr_color_conversion *self) {
    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            if(load_graphics_shader_y(&self->graphics_shaders[GRAPHICS_SHADER_INDEX_Y], self->params.egl, &self->graphics_uniforms[GRAPHICS_SHADER_INDEX_Y], self->params.destination_color, self->params.color_range, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y graphics shader\n");
                return false;
            }

            if(load_graphics_shader_uv(&self->graphics_shaders[GRAPHICS_SHADER_INDEX_UV], self->params.egl, &self->graphics_uniforms[GRAPHICS_SHADER_INDEX_UV], self->params.destination_color, self->params.color_range, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV graphics shader\n");
                return false;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            if(load_graphics_shader_rgb(&self->graphics_shaders[GRAPHICS_SHADER_INDEX_RGB], self->params.egl, &self->graphics_uniforms[GRAPHICS_SHADER_INDEX_RGB], false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y graphics shader\n");
                return false;
            }
            break;
        }
    }
    return true;
}

static bool gsr_color_conversion_load_external_graphics_shaders(gsr_color_conversion *self) {
    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            if(load_graphics_shader_y(&self->graphics_shaders[GRAPHICS_SHADER_INDEX_Y_EXTERNAL], self->params.egl, &self->graphics_uniforms[GRAPHICS_SHADER_INDEX_Y_EXTERNAL], self->params.destination_color, self->params.color_range, true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y graphics shader\n");
                return false;
            }

            if(load_graphics_shader_uv(&self->graphics_shaders[GRAPHICS_SHADER_INDEX_UV_EXTERNAL], self->params.egl, &self->graphics_uniforms[GRAPHICS_SHADER_INDEX_UV_EXTERNAL], self->params.destination_color, self->params.color_range, true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV graphics shader\n");
                return false;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            if(load_graphics_shader_rgb(&self->graphics_shaders[GRAPHICS_SHADER_INDEX_RGB_EXTERNAL], self->params.egl, &self->graphics_uniforms[GRAPHICS_SHADER_INDEX_RGB_EXTERNAL], true) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y graphics shader\n");
                return false;
            }
            break;
        }
    }
    return true;
}

int gsr_color_conversion_init(gsr_color_conversion *self, const gsr_color_conversion_params *params) {
    assert(params);
    assert(params->egl);
    memset(self, 0, sizeof(*self));
    self->params.egl = params->egl;
    self->params = *params;
    
    int max_compute_work_group_invocations = 256;
    self->params.egl->glGetIntegerv(GL_MAX_COMPUTE_FIXED_GROUP_INVOCATIONS, &max_compute_work_group_invocations);
    self->max_local_size_dim = sqrt(max_compute_work_group_invocations);

    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            if(self->params.num_destination_textures != 2) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: expected 2 destination textures for destination color NV12/P010, got %d destination texture(s)\n", self->params.num_destination_textures);
                goto err;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            if(self->params.num_destination_textures != 1) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: expected 1 destination textures for destination color RGB8, got %d destination texture(s)\n", self->params.num_destination_textures);
                goto err;
            }
            break;
        }
    }

    if(self->params.force_graphics_shader) {
        self->compute_shaders_failed_to_load = true;
        self->external_compute_shaders_failed_to_load = true;
        
        if(!gsr_color_conversion_load_graphics_shaders(self))
            goto err;

        if(self->params.load_external_image_shader) {
            if(!gsr_color_conversion_load_external_graphics_shaders(self))
                goto err;
        }
    } else {
        if(!gsr_color_conversion_load_compute_shaders(self)) {
            self->compute_shaders_failed_to_load = true;
            fprintf(stderr, "gsr info: failed to load one or more compute shaders, run gpu-screen-recorder with the '-gl-debug yes' option to see why. Falling back to slower graphics shader instead\n");
            if(!gsr_color_conversion_load_graphics_shaders(self))
                goto err;
        }

        if(self->params.load_external_image_shader) {
            if(!gsr_color_conversion_load_external_compute_shaders(self)) {
                self->external_compute_shaders_failed_to_load = true;
                fprintf(stderr, "gsr info: failed to load one or more external compute shaders, run gpu-screen-recorder with the '-gl-debug yes' option to see why. Falling back to slower graphics shader instead\n");
                if(!gsr_color_conversion_load_external_graphics_shaders(self))
                    goto err;
            }
        }
    }

    if(load_framebuffers(self) != 0)
        goto err;

    if(create_vertices(self) != 0)
        goto err;

    return 0;

    err:
    gsr_color_conversion_deinit(self);
    return -1;
}

void gsr_color_conversion_deinit(gsr_color_conversion *self) {
    if(!self->params.egl)
        return;

    if(self->vertex_buffer_object_id) {
        self->params.egl->glDeleteBuffers(1, &self->vertex_buffer_object_id);
        self->vertex_buffer_object_id = 0;
    }

    if(self->vertex_array_object_id) {
        self->params.egl->glDeleteVertexArrays(1, &self->vertex_array_object_id);
        self->vertex_array_object_id = 0;
    }

    self->params.egl->glDeleteFramebuffers(GSR_COLOR_CONVERSION_MAX_FRAMEBUFFERS, self->framebuffers);
    for(int i = 0; i < GSR_COLOR_CONVERSION_MAX_FRAMEBUFFERS; ++i) {
        self->framebuffers[i] = 0;
    }

    for(int i = 0; i < GSR_COLOR_CONVERSION_MAX_COMPUTE_SHADERS; ++i) {
        gsr_shader_deinit(&self->compute_shaders[i]);
    }

    for(int i = 0; i < GSR_COLOR_CONVERSION_MAX_GRAPHICS_SHADERS; ++i) {
        gsr_shader_deinit(&self->graphics_shaders[i]);
    }

    self->params.egl = NULL;
}

static void gsr_color_conversion_apply_rotation(gsr_rotation rotation, float rotation_matrix[2][2]) {
    /*
    rotation_matrix[0][0] =  cos(angle);
    rotation_matrix[0][1] = -sin(angle);
    rotation_matrix[1][0] =  sin(angle);
    rotation_matrix[1][1] =  cos(angle);
    The manual matrix code below is the same as this code above, but without floating-point errors.
    This is done to remove any blurring caused by these floating-point errors.
    */
    switch(rotation) {
        case GSR_ROT_0:
            rotation_matrix[0][0] = 1.0f;
            rotation_matrix[0][1] = 0.0f;
            rotation_matrix[1][0] = 0.0f;
            rotation_matrix[1][1] = 1.0f;
            break;
        case GSR_ROT_90:
            rotation_matrix[0][0] =  0.0f;
            rotation_matrix[0][1] = -1.0f;
            rotation_matrix[1][0] =  1.0f;
            rotation_matrix[1][1] =  0.0f;
            break;
        case GSR_ROT_180:
            rotation_matrix[0][0] = -1.0f;
            rotation_matrix[0][1] =  0.0f;
            rotation_matrix[1][0] =  0.0f;
            rotation_matrix[1][1] = -1.0f;
            break;
        case GSR_ROT_270:
            rotation_matrix[0][0] =  0.0f;
            rotation_matrix[0][1] =  1.0f;
            rotation_matrix[1][0] = -1.0f;
            rotation_matrix[1][1] =  0.0f;
            break;
    }
}

static void gsr_color_conversion_swizzle_texture_source(gsr_color_conversion *self, gsr_source_color source_color) {
    if(source_color == GSR_SOURCE_COLOR_BGR) {
        const int swizzle_mask[] = { GL_BLUE, GL_GREEN, GL_RED, 1 };
        self->params.egl->glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);
    }
}

static void gsr_color_conversion_swizzle_reset(gsr_color_conversion *self, gsr_source_color source_color) {
    if(source_color == GSR_SOURCE_COLOR_BGR) {
        const int swizzle_mask[] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
        self->params.egl->glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);
    }
}

typedef enum {
    GSR_COLOR_COMP_Y,
    GSR_COLOR_COMP_UV,
    GSR_COLOR_COMP_RGB
} gsr_color_component;

static int color_component_get_destination_texture_index(gsr_color_component color_component) {
    switch(color_component) {
        case GSR_COLOR_COMP_Y:   return 0;
        case GSR_COLOR_COMP_UV:  return 1;
        case GSR_COLOR_COMP_RGB: return 0;
    }
    assert(false);
    return 0;
}

static unsigned int color_component_get_color_format(gsr_color_component color_component, bool use_16bit_colors) {
    switch(color_component) {
        case GSR_COLOR_COMP_Y:   return use_16bit_colors ? GL_R16 : GL_R8;
        case GSR_COLOR_COMP_UV:  return use_16bit_colors ? GL_RG16 : GL_RG8;
        case GSR_COLOR_COMP_RGB: return GL_RGBA8; // TODO: 16-bit color support
    }
    assert(false);
    return GL_RGBA8;
}

static int color_component_get_COMPUTE_SHADER_INDEX(gsr_color_component color_component, bool external_texture, bool alpha_blending) {
    switch(color_component) {
        case GSR_COLOR_COMP_Y: {
            if(external_texture)
                return alpha_blending ? COMPUTE_SHADER_INDEX_Y_EXTERNAL_BLEND : COMPUTE_SHADER_INDEX_Y_EXTERNAL;
            else
                return alpha_blending ? COMPUTE_SHADER_INDEX_Y_BLEND : COMPUTE_SHADER_INDEX_Y;
        }
        case GSR_COLOR_COMP_UV: {
            if(external_texture)
                return alpha_blending ? COMPUTE_SHADER_INDEX_UV_EXTERNAL_BLEND : COMPUTE_SHADER_INDEX_UV_EXTERNAL;
            else
                return alpha_blending ? COMPUTE_SHADER_INDEX_UV_BLEND : COMPUTE_SHADER_INDEX_UV;
        }
        case GSR_COLOR_COMP_RGB: {
            if(external_texture)
                return alpha_blending ? COMPUTE_SHADER_INDEX_RGB_EXTERNAL_BLEND : COMPUTE_SHADER_INDEX_RGB_EXTERNAL;
            else
                return alpha_blending ? COMPUTE_SHADER_INDEX_RGB_BLEND : COMPUTE_SHADER_INDEX_RGB;
        }
    }
    assert(false);
    return COMPUTE_SHADER_INDEX_RGB;
}

static void gsr_color_conversion_dispatch_compute_shader(gsr_color_conversion *self, bool external_texture, bool alpha_blending, float rotation_matrix[2][2], vec2i source_position, vec2i destination_pos, vec2i destination_size, vec2f scale, bool use_16bit_colors, gsr_color_component color_component) {
    const int compute_shader_index = color_component_get_COMPUTE_SHADER_INDEX(color_component, external_texture, alpha_blending);
    const int destination_texture_index = color_component_get_destination_texture_index(color_component);
    const unsigned int color_format = color_component_get_color_format(color_component, use_16bit_colors);

    self->params.egl->glActiveTexture(GL_TEXTURE1);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->params.destination_textures[destination_texture_index]);
    self->params.egl->glActiveTexture(GL_TEXTURE0);

    gsr_color_compute_uniforms *uniform = &self->compute_uniforms[compute_shader_index];
    gsr_shader_use(&self->compute_shaders[compute_shader_index]);
    self->params.egl->glUniformMatrix2fv(uniform->rotation_matrix, 1, GL_TRUE, (const float*)rotation_matrix);
    self->params.egl->glUniform2i(uniform->source_position, source_position.x, source_position.y);
    self->params.egl->glUniform2i(uniform->target_position, destination_pos.x, destination_pos.y);
    self->params.egl->glUniform2f(uniform->scale, scale.x, scale.y);
    self->params.egl->glBindImageTexture(0, self->params.destination_textures[destination_texture_index], 0, GL_FALSE, 0, GL_WRITE_ONLY, color_format);
    const double num_groups_x = ceil((double)destination_size.x/(double)self->max_local_size_dim);
    const double num_groups_y = ceil((double)destination_size.y/(double)self->max_local_size_dim);
    self->params.egl->glDispatchCompute(max_int(1, num_groups_x), max_int(1, num_groups_y), 1);
}

static void gsr_color_conversion_draw_graphics(gsr_color_conversion *self, unsigned int texture_id, bool external_texture, float rotation_matrix[2][2], vec2i source_position, vec2i source_size, vec2i destination_pos, vec2i texture_size, vec2f scale, gsr_source_color source_color) {
    /* TODO: Do not call this every frame? */
    vec2i dest_texture_size = {0, 0};
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->params.destination_textures[0]);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &dest_texture_size.x);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &dest_texture_size.y);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    const int texture_target = external_texture ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;

    self->params.egl->glBindTexture(texture_target, texture_id);
    gsr_color_conversion_swizzle_texture_source(self, source_color);

    const vec2f pos_norm = {
        ((float)destination_pos.x / (dest_texture_size.x == 0 ? 1.0f : (float)dest_texture_size.x)) * 2.0f,
        ((float)destination_pos.y / (dest_texture_size.y == 0 ? 1.0f : (float)dest_texture_size.y)) * 2.0f,
    };

    const vec2f size_norm = {
        ((float)source_size.x / (dest_texture_size.x == 0 ? 1.0f : (float)dest_texture_size.x)) * 2.0f * scale.x,
        ((float)source_size.y / (dest_texture_size.y == 0 ? 1.0f : (float)dest_texture_size.y)) * 2.0f * scale.y,
    };

    const vec2f texture_pos_norm = {
        (float)source_position.x / (texture_size.x == 0 ? 1.0f : (float)texture_size.x),
        (float)source_position.y / (texture_size.y == 0 ? 1.0f : (float)texture_size.y),
    };

    const vec2f texture_size_norm = {
        (float)source_size.x / (texture_size.x == 0 ? 1.0f : (float)texture_size.x),
        (float)source_size.y / (texture_size.y == 0 ? 1.0f : (float)texture_size.y),
    };

    const float vertices[] = {
        -1.0f + 0.0f,               -1.0f + 0.0f + size_norm.y, texture_pos_norm.x,                       texture_pos_norm.y + texture_size_norm.y,
        -1.0f + 0.0f,               -1.0f + 0.0f,               texture_pos_norm.x,                       texture_pos_norm.y,
        -1.0f + 0.0f + size_norm.x, -1.0f + 0.0f,               texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y,

        -1.0f + 0.0f,               -1.0f + 0.0f + size_norm.y, texture_pos_norm.x,                       texture_pos_norm.y + texture_size_norm.y,
        -1.0f + 0.0f + size_norm.x, -1.0f + 0.0f,               texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y,
        -1.0f + 0.0f + size_norm.x, -1.0f + 0.0f + size_norm.y, texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y + texture_size_norm.y
    };

    self->params.egl->glBindVertexArray(self->vertex_array_object_id);
    self->params.egl->glViewport(0, 0, dest_texture_size.x, dest_texture_size.y);

    /* TODO: this, also cleanup */
    self->params.egl->glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer_object_id);
    self->params.egl->glBufferSubData(GL_ARRAY_BUFFER, 0, 24 * sizeof(float), vertices);

    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
            //cap_xcomp->params.egl->glClear(GL_COLOR_BUFFER_BIT); // TODO: Do this in a separate clear_ function. We want to do that when using multiple drm to create the final image (multiple monitors for example)

            int shader_index = external_texture ? GRAPHICS_SHADER_INDEX_Y_EXTERNAL : GRAPHICS_SHADER_INDEX_Y;
            gsr_shader_use(&self->graphics_shaders[shader_index]);
            self->params.egl->glUniformMatrix2fv(self->graphics_uniforms[shader_index].rotation_matrix, 1, GL_TRUE, (const float*)rotation_matrix);
            self->params.egl->glUniform2f(self->graphics_uniforms[shader_index].offset, pos_norm.x, pos_norm.y);
            self->params.egl->glDrawArrays(GL_TRIANGLES, 0, 6);

            if(self->params.num_destination_textures > 1) {
                self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
                //cap_xcomp->params.egl->glClear(GL_COLOR_BUFFER_BIT);

                shader_index = external_texture ? GRAPHICS_SHADER_INDEX_UV_EXTERNAL : GRAPHICS_SHADER_INDEX_UV;
                gsr_shader_use(&self->graphics_shaders[shader_index]);
                self->params.egl->glUniformMatrix2fv(self->graphics_uniforms[shader_index].rotation_matrix, 1, GL_TRUE, (const float*)rotation_matrix);
                self->params.egl->glUniform2f(self->graphics_uniforms[shader_index].offset, pos_norm.x, pos_norm.y);
                self->params.egl->glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
            //cap_xcomp->params.egl->glClear(GL_COLOR_BUFFER_BIT); // TODO: Do this in a separate clear_ function. We want to do that when using multiple drm to create the final image (multiple monitors for example)

            const int shader_index = external_texture ? GRAPHICS_SHADER_INDEX_RGB_EXTERNAL : GRAPHICS_SHADER_INDEX_RGB;
            gsr_shader_use(&self->graphics_shaders[shader_index]);
            self->params.egl->glUniformMatrix2fv(self->graphics_uniforms[shader_index].rotation_matrix, 1, GL_TRUE, (const float*)rotation_matrix);
            self->params.egl->glUniform2f(self->graphics_uniforms[shader_index].offset, pos_norm.x, pos_norm.y);
            self->params.egl->glDrawArrays(GL_TRIANGLES, 0, 6);
            break;
        }
    }

    self->params.egl->glBindVertexArray(0);
    self->params.egl->glUseProgram(0);
    gsr_color_conversion_swizzle_reset(self, source_color);
    self->params.egl->glBindTexture(texture_target, 0);
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gsr_color_conversion_draw(gsr_color_conversion *self, unsigned int texture_id, vec2i destination_pos, vec2i destination_size, vec2i source_pos, vec2i source_size, vec2i texture_size, gsr_rotation rotation, gsr_source_color source_color, bool external_texture, bool alpha_blending) {
    assert(!external_texture || self->params.load_external_image_shader);
    if(external_texture && !self->params.load_external_image_shader) {
        fprintf(stderr, "gsr error: gsr_color_conversion_draw: external texture not loaded\n");
        return;
    }

    vec2f scale = {0.0f, 0.0f};
    if(source_size.x > 0 && source_size.y > 0)
        scale = (vec2f){ (double)destination_size.x/(double)source_size.x, (double)destination_size.y/(double)source_size.y };

    vec2i source_position = {0, 0};
    float rotation_matrix[2][2] = {{0, 0}, {0, 0}};
    gsr_color_conversion_apply_rotation(rotation, rotation_matrix);

    const int texture_target = external_texture ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    self->params.egl->glBindTexture(texture_target, texture_id);
    gsr_color_conversion_swizzle_texture_source(self, source_color);

    const bool use_graphics_shader = external_texture ? self->external_compute_shaders_failed_to_load : self->compute_shaders_failed_to_load;
    if(use_graphics_shader) {
        source_position.x += source_pos.x;
        source_position.y += source_pos.y;
        gsr_color_conversion_draw_graphics(self, texture_id, external_texture, rotation_matrix, source_position, source_size, destination_pos, texture_size, scale, source_color);
    } else {
        switch(rotation) {
            case GSR_ROT_0:
                break;
            case GSR_ROT_90:
                source_position.x += (((double)texture_size.x*0.5 - (double)texture_size.y*0.5) * scale.x);
                source_position.y += (((double)texture_size.y*0.5 - (double)texture_size.x*0.5) * scale.y);
                break;
            case GSR_ROT_180:
                break;
            case GSR_ROT_270:
                source_position.x += (((double)texture_size.x*0.5 - (double)texture_size.y*0.5) * scale.x);
                source_position.y += (((double)texture_size.y*0.5 - (double)texture_size.x*0.5) * scale.y);
                break;
        }
        source_position.x -= (source_pos.x * scale.x + 0.5);
        source_position.y -= (source_pos.y * scale.y + 0.5);

        switch(self->params.destination_color) {
            case GSR_DESTINATION_COLOR_NV12:
            case GSR_DESTINATION_COLOR_P010: {
                const bool use_16bit_colors = self->params.destination_color == GSR_DESTINATION_COLOR_P010;
                gsr_color_conversion_dispatch_compute_shader(self, external_texture, alpha_blending, rotation_matrix, source_position, destination_pos, destination_size, scale, use_16bit_colors, GSR_COLOR_COMP_Y);
                gsr_color_conversion_dispatch_compute_shader(self, external_texture, alpha_blending, rotation_matrix, (vec2i){source_position.x/2, source_position.y/2},
                    (vec2i){destination_pos.x/2, destination_pos.y/2}, (vec2i){destination_size.x/2, destination_size.y/2}, scale, use_16bit_colors, GSR_COLOR_COMP_UV);
                break;
            }
            case GSR_DESTINATION_COLOR_RGB8: {
                gsr_color_conversion_dispatch_compute_shader(self, external_texture, alpha_blending, rotation_matrix, source_position, destination_pos, destination_size, scale, false, GSR_COLOR_COMP_RGB);
                break;
            }
        }
    }

    self->params.egl->glFlush();
    // TODO: Use the minimal barrier required
    self->params.egl->glMemoryBarrier(GL_ALL_BARRIER_BITS); // GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
    self->params.egl->glUseProgram(0);

    gsr_color_conversion_swizzle_reset(self, source_color);
    self->params.egl->glBindTexture(texture_target, 0);
}

void gsr_color_conversion_clear(gsr_color_conversion *self) {
    float color1[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float color2[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            color2[0] = 0.5f;
            color2[1] = 0.5f;
            color2[2] = 0.0f;
            color2[3] = 1.0f;
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            color2[0] = 0.0f;
            color2[1] = 0.0f;
            color2[2] = 0.0f;
            color2[3] = 1.0f;
            break;
        }
    }

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
    self->params.egl->glClearColor(color1[0], color1[1], color1[2], color1[3]);
    self->params.egl->glClear(GL_COLOR_BUFFER_BIT);

    if(self->params.num_destination_textures > 1) {
        self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
        self->params.egl->glClearColor(color2[0], color2[1], color2[2], color2[3]);
        self->params.egl->glClear(GL_COLOR_BUFFER_BIT);
    }

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gsr_color_conversion_read_destination_texture(gsr_color_conversion *self, int destination_texture_index, int x, int y, int width, int height, unsigned int color_format, unsigned int data_format, void *pixels) {
    assert(destination_texture_index >= 0 && destination_texture_index < self->params.num_destination_textures);
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[destination_texture_index]);
    self->params.egl->glReadPixels(x, y, width, height, color_format, data_format, pixels);
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

gsr_rotation gsr_monitor_rotation_to_rotation(gsr_monitor_rotation monitor_rotation) {
    return (gsr_rotation)monitor_rotation;
}
