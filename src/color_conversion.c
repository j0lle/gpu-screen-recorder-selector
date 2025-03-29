#include "../include/color_conversion.h"
#include "../include/egl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

// TODO: external texture
// TODO: Scissor doesn't work with compute shader. In the compute shader this can be implemented with two step calls, and using the result
// with a call to mix to choose source/output color.

#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
// TODO: Use the minimal barrier required and move this to egl.h
#define GL_ALL_BARRIER_BITS               0xFFFFFFFF

#define MAX_FRAMEBUFFERS 2
#define EXTERNAL_TEXTURE_SHADER_OFFSET 2

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

// TODO: Make alpha blending optional
// TODO: Optimize these shaders.
static int load_compute_shader_y(gsr_shader *shader, gsr_egl *egl, gsr_color_uniforms *uniforms, int max_local_size_dim, gsr_destination_color color_format, gsr_color_range color_range) {
    const char *color_transform_matrix = color_format_range_get_transform_matrix(color_format, color_range);
    const bool use_16bit_colors = color_format == GSR_DESTINATION_COLOR_P010;

    char compute_shader[2048];
    snprintf(compute_shader, sizeof(compute_shader),
        "#version 430 core\n"
        "layout (local_size_x = %d, local_size_y = %d, local_size_z = 1) in;\n"
        "uniform sampler2D imgInput;\n"
        "uniform ivec2 source_position;\n"
        "uniform ivec2 target_position;\n"
        "uniform vec2 scale;\n"
        "uniform mat2 rotation_matrix;\n"
        "layout(%s, binding = 0) uniform image2D imgOutput;\n"
        "%s"
        "void main() {\n"
        "    ivec2 texelCoord = ivec2(gl_GlobalInvocationID.xy);\n"
        "    ivec2 size = ivec2(vec2(textureSize(imgInput, 0)) * scale + 0.5);\n"
        "    vec2 rotated_texel_coord = vec2(texelCoord - source_position - size/2) * rotation_matrix + vec2(size/2) + 0.5;\n"
        "    vec2 texCoord = vec2(rotated_texel_coord)/vec2(size);\n"
        "    vec4 source_color = texture(imgInput, texCoord);\n"
        "    vec4 source_color_yuv = RGBtoYUV * vec4(source_color.rgb, 1.0);\n"
        "    vec4 output_color_yuv = imageLoad(imgOutput, ivec2(rotated_texel_coord) + target_position);\n"
        "    float y_color = mix(output_color_yuv.r, source_color_yuv.r, source_color.a);\n"
        "    imageStore(imgOutput, texelCoord + target_position, vec4(y_color, 1.0, 1.0, 1.0));\n"
        "}\n", max_local_size_dim, max_local_size_dim, use_16bit_colors ? "r16" : "r8", color_transform_matrix);

    if(gsr_shader_init(shader, egl, NULL, NULL, compute_shader) != 0)
        return -1;

    uniforms->source_position = egl->glGetUniformLocation(shader->program_id, "source_position");
    uniforms->target_position = egl->glGetUniformLocation(shader->program_id, "target_position");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    uniforms->scale = egl->glGetUniformLocation(shader->program_id, "scale");
    return 0;
}

static int load_compute_shader_uv(gsr_shader *shader, gsr_egl *egl, gsr_color_uniforms *uniforms, int max_local_size_dim, gsr_destination_color color_format, gsr_color_range color_range) {
    const char *color_transform_matrix = color_format_range_get_transform_matrix(color_format, color_range);
    const bool use_16bit_colors = color_format == GSR_DESTINATION_COLOR_P010;

    char compute_shader[2048];
    snprintf(compute_shader, sizeof(compute_shader),
        "#version 430 core\n"
        "layout (local_size_x = %d, local_size_y = %d, local_size_z = 1) in;\n"
        "uniform sampler2D imgInput;\n"
        "uniform ivec2 source_position;\n"
        "uniform ivec2 target_position;\n"
        "uniform vec2 scale;\n"
        "uniform mat2 rotation_matrix;\n"
        "layout(%s, binding = 0) uniform image2D imgOutput;\n"
        "%s"
        "void main() {\n"
        "    ivec2 texelCoord = ivec2(gl_GlobalInvocationID.xy);\n"
        "    ivec2 size = ivec2(vec2(textureSize(imgInput, 0)) * scale + 0.5);\n"
        "    vec2 rotated_texel_coord = vec2(texelCoord - source_position/2 - size/4) * rotation_matrix + vec2(size/4) + 0.5;\n"
        "    vec2 texCoord = vec2(rotated_texel_coord)/vec2(size);\n"
        "    vec4 source_color = texture(imgInput, texCoord * 2.0);\n"
        "    vec4 source_color_yuv = RGBtoYUV * vec4(source_color.rgb, 1.0);\n"
        "    vec4 output_color_yuv = imageLoad(imgOutput, ivec2(rotated_texel_coord) + target_position/2);\n"
        "    vec2 uv_color = mix(output_color_yuv.rg, source_color_yuv.gb, source_color.a);\n"
        "    imageStore(imgOutput, texelCoord + target_position/2, vec4(uv_color, 1.0, 1.0));\n"
        "}\n", max_local_size_dim, max_local_size_dim, use_16bit_colors ? "rg16" : "rg8", color_transform_matrix);

    if(gsr_shader_init(shader, egl, NULL, NULL, compute_shader) != 0)
        return -1;

    uniforms->source_position = egl->glGetUniformLocation(shader->program_id, "source_position");
    uniforms->target_position = egl->glGetUniformLocation(shader->program_id, "target_position");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    uniforms->scale = egl->glGetUniformLocation(shader->program_id, "scale");
    return 0;
}

static int load_compute_shader_rgb(gsr_shader *shader, gsr_egl *egl, gsr_color_uniforms *uniforms, int max_local_size_dim) {
    char compute_shader[2048];
    snprintf(compute_shader, sizeof(compute_shader),
        "#version 430 core\n"
        "layout (local_size_x = %d, local_size_y = %d, local_size_z = 1) in;\n"
        "uniform sampler2D imgInput;\n"
        "uniform ivec2 source_position;\n"
        "uniform ivec2 target_position;\n"
        "uniform vec2 scale;\n"
        "uniform mat2 rotation_matrix;\n"
        "layout(rgba8, binding = 0) uniform image2D imgOutput;\n"
        "void main() {\n"
        "    ivec2 texelCoord = ivec2(gl_GlobalInvocationID.xy);\n"
        "    ivec2 size = ivec2(vec2(textureSize(imgInput, 0)) * scale + 0.5);\n"
        "    vec2 rotated_texel_coord = vec2(texelCoord - source_position - size/2) * rotation_matrix + vec2(size/2) + 0.5;\n"
        "    vec2 texCoord = vec2(rotated_texel_coord)/vec2(size);\n"
        "    vec4 source_color = texture(imgInput, texCoord);\n"
        //"    vec4 output_color = imageLoad(imgOutput, ivec2(rotated_texel_coord) + target_position);\n"
        //"    vec3 color = mix(output_color.rgb, source_color.rgb, source_color.a);\n"
        "    imageStore(imgOutput, texelCoord + target_position, source_color);\n"
        "}\n", max_local_size_dim, max_local_size_dim);

    if(gsr_shader_init(shader, egl, NULL, NULL, compute_shader) != 0)
        return -1;

    uniforms->source_position = egl->glGetUniformLocation(shader->program_id, "source_position");
    uniforms->target_position = egl->glGetUniformLocation(shader->program_id, "target_position");
    uniforms->rotation_matrix = egl->glGetUniformLocation(shader->program_id, "rotation_matrix");
    uniforms->scale = egl->glGetUniformLocation(shader->program_id, "scale");
    return 0;
}

static int load_framebuffers(gsr_color_conversion *self) {
    /* TODO: Only generate the necessary amount of framebuffers (self->params.num_destination_textures) */
    const unsigned int draw_buffer = GL_COLOR_ATTACHMENT0;
    self->params.egl->glGenFramebuffers(MAX_FRAMEBUFFERS, self->framebuffers);

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

int gsr_color_conversion_init(gsr_color_conversion *self, const gsr_color_conversion_params *params) {
    assert(params);
    assert(params->egl);
    memset(self, 0, sizeof(*self));
    self->params.egl = params->egl;
    self->params = *params;
    
    int max_compute_work_group_invocations = 256;
    self->params.egl->glGetIntegerv(GL_MAX_COMPUTE_FIXED_GROUP_INVOCATIONS, &max_compute_work_group_invocations);
    self->max_local_size_dim = sqrt(max_compute_work_group_invocations);
    fprintf(stderr, "max local size: %d, max_local_size_dim: %d\n", max_compute_work_group_invocations, self->max_local_size_dim);

    switch(params->destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            if(self->params.num_destination_textures != 2) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: expected 2 destination textures for destination color NV12/P010, got %d destination texture(s)\n", self->params.num_destination_textures);
                return -1;
            }

            if(load_compute_shader_y(&self->shaders[0], self->params.egl, &self->uniforms[0], self->max_local_size_dim, params->destination_color, params->color_range) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                goto err;
            }

            if(load_compute_shader_uv(&self->shaders[1], self->params.egl, &self->uniforms[1], self->max_local_size_dim, params->destination_color, params->color_range) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV compute shader\n");
                goto err;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            if(self->params.num_destination_textures != 1) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: expected 1 destination textures for destination color RGB8, got %d destination texture(s)\n", self->params.num_destination_textures);
                return -1;
            }

            if(load_compute_shader_rgb(&self->shaders[2], self->params.egl, &self->uniforms[2], self->max_local_size_dim) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y compute shader\n");
                goto err;
            }
            break;
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

    self->params.egl->glDeleteFramebuffers(MAX_FRAMEBUFFERS, self->framebuffers);
    for(int i = 0; i < MAX_FRAMEBUFFERS; ++i) {
        self->framebuffers[i] = 0;
    }

    for(int i = 0; i < GSR_COLOR_CONVERSION_MAX_SHADERS; ++i) {
        gsr_shader_deinit(&self->shaders[i]);
    }

    self->params.egl = NULL;
}

static void gsr_color_conversion_apply_rotation(gsr_rotation rotation, float rotation_matrix[2][2], vec2i *source_position, vec2i texture_size, vec2f scale) {
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
            source_position->x += (((double)texture_size.x*0.5 - (double)texture_size.y*0.5) * scale.x + 0.5);
            source_position->y += (((double)texture_size.y*0.5 - (double)texture_size.x*0.5) * scale.y + 0.5);
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
            source_position->x += (((double)texture_size.x*0.5 - (double)texture_size.y*0.5) * scale.x + 0.5);
            source_position->y += (((double)texture_size.y*0.5 - (double)texture_size.x*0.5) * scale.y + 0.5);
            break;
    }
}

// TODO: Handle source_color
void gsr_color_conversion_draw(gsr_color_conversion *self, unsigned int texture_id, vec2i destination_pos, vec2i destination_size, vec2i texture_pos, vec2i texture_size, gsr_rotation rotation, bool external_texture, gsr_source_color source_color) {
    vec2f scale = {0.0f, 0.0f};
    if(texture_size.x > 0 && texture_size.y > 0)
        scale = (vec2f){ (double)destination_size.x/(double)texture_size.x, (double)destination_size.y/(double)texture_size.y };

    vec2i source_position = {0, 0};
    float rotation_matrix[2][2] = {{0, 0}, {0, 0}};
    gsr_color_conversion_apply_rotation(rotation, rotation_matrix, &source_position, texture_size, scale);

    source_position.x += texture_pos.x;
    source_position.y += texture_pos.y;

    const int texture_target = external_texture ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    self->params.egl->glBindTexture(texture_target, texture_id);

    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            const bool use_16bit_colors = self->params.destination_color == GSR_DESTINATION_COLOR_P010;
            // Y
            {
                gsr_shader_use(&self->shaders[0]);
                self->params.egl->glUniformMatrix2fv(self->uniforms[0].rotation_matrix, 1, GL_TRUE, (const float*)rotation_matrix);
                self->params.egl->glUniform2i(self->uniforms[0].source_position, source_position.x, source_position.y);
                self->params.egl->glUniform2i(self->uniforms[0].target_position, destination_pos.x, destination_pos.y);
                self->params.egl->glUniform2f(self->uniforms[0].scale, scale.x, scale.y);
                self->params.egl->glBindImageTexture(0, self->params.destination_textures[0], 0, GL_FALSE, 0, GL_READ_WRITE, use_16bit_colors ? GL_R16 : GL_R8);
                const double num_groups_x = (double)texture_size.x/(double)self->max_local_size_dim + 0.5;
                const double num_groups_y = (double)texture_size.y/(double)self->max_local_size_dim + 0.5;
                self->params.egl->glDispatchCompute(max_int(1, num_groups_x), max_int(1, num_groups_y), 1);
            }

            // UV
            {
                gsr_shader_use(&self->shaders[1]);
                self->params.egl->glUniformMatrix2fv(self->uniforms[1].rotation_matrix, 1, GL_TRUE, (const float*)rotation_matrix);
                self->params.egl->glUniform2i(self->uniforms[1].source_position, source_position.x, source_position.y);
                self->params.egl->glUniform2i(self->uniforms[1].target_position, destination_pos.x, destination_pos.y);
                self->params.egl->glUniform2f(self->uniforms[1].scale, scale.x, scale.y);
                self->params.egl->glBindImageTexture(0, self->params.destination_textures[1], 0, GL_FALSE, 0, GL_READ_WRITE, use_16bit_colors ? GL_RG16 : GL_RG8);
                const double num_groups_x = (double)texture_size.x*0.5/(double)self->max_local_size_dim + 0.5;
                const double num_groups_y = (double)texture_size.y*0.5/(double)self->max_local_size_dim + 0.5;
                self->params.egl->glDispatchCompute(max_int(1, num_groups_x), max_int(1, num_groups_y), 1);
            }
            break;
        }
        case GSR_DESTINATION_COLOR_RGB8: {
            gsr_shader_use(&self->shaders[2]);
            self->params.egl->glUniformMatrix2fv(self->uniforms[2].rotation_matrix, 1, GL_TRUE, (const float*)rotation_matrix);
            self->params.egl->glUniform2i(self->uniforms[2].source_position, source_position.x, source_position.y);
            self->params.egl->glUniform2i(self->uniforms[2].target_position, destination_pos.x, destination_pos.y);
            self->params.egl->glUniform2f(self->uniforms[2].scale, scale.x, scale.y);
            self->params.egl->glBindImageTexture(0, self->params.destination_textures[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
            const double num_groups_x = (double)texture_size.x/(double)self->max_local_size_dim + 0.5;
            const double num_groups_y = (double)texture_size.y/(double)self->max_local_size_dim + 0.5;
            self->params.egl->glDispatchCompute(max_int(1, num_groups_x), max_int(1, num_groups_y), 1);
            break;
        }
    }

    self->params.egl->glMemoryBarrier(GL_ALL_BARRIER_BITS); // GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
    self->params.egl->glUseProgram(0);

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

gsr_rotation gsr_monitor_rotation_to_rotation(gsr_monitor_rotation monitor_rotation) {
    return (gsr_rotation)monitor_rotation;
}
