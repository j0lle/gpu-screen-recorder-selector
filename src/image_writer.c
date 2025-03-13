#include "../include/image_writer.h"
#include "../include/egl.h"
#include "../include/utils.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../external/stb_image_write.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

/* TODO: Support hdr/10-bit */
bool gsr_image_writer_init_opengl(gsr_image_writer *self, gsr_egl *egl, int width, int height) {
    memset(self, 0, sizeof(*self));
    self->source = GSR_IMAGE_WRITER_SOURCE_OPENGL;
    self->egl = egl;
    self->width = width;
    self->height = height;
    self->texture = gl_create_texture(self->egl, self->width, self->height, GL_RGB8, GL_RGB, GL_NEAREST); /* TODO: use GL_RGB16 instead of GL_RGB8 for hdr/10-bit */
    if(self->texture == 0) {
        fprintf(stderr, "gsr error: gsr_image_writer_init: failed to create texture\n");
        return false;
    }
    return true;
}

bool gsr_image_writer_init_memory(gsr_image_writer *self, const void *memory, int width, int height) {
    memset(self, 0, sizeof(*self));
    self->source = GSR_IMAGE_WRITER_SOURCE_OPENGL;
    self->width = width;
    self->height = height;
    self->memory = memory;
    return true;
}

void gsr_image_writer_deinit(gsr_image_writer *self) {
    if(self->texture) {
        self->egl->glDeleteTextures(1, &self->texture);
        self->texture = 0;
    }
}

static bool gsr_image_writer_write_memory_to_file(gsr_image_writer *self, const char *filepath, gsr_image_format image_format, int quality, const void *data) {
    if(quality < 1)
        quality = 1;
    else if(quality > 100)
        quality = 100;

    bool success = false;
    switch(image_format) {
        case GSR_IMAGE_FORMAT_JPEG:
            success = stbi_write_jpg(filepath, self->width, self->height, 3, data, quality);
            break;
        case GSR_IMAGE_FORMAT_PNG:
            success = stbi_write_png(filepath, self->width, self->height, 3, data, 0);
            break;
    }

    if(!success)
        fprintf(stderr, "gsr error: gsr_image_writer_write_to_file: failed to write image data to output file %s\n", filepath);

    return success;
}

static bool gsr_image_writer_write_opengl_texture_to_file(gsr_image_writer *self, const char *filepath, gsr_image_format image_format, int quality) {
    assert(self->source == GSR_IMAGE_WRITER_SOURCE_OPENGL);
    uint8_t *frame_data = malloc(self->width * self->height * 3);
    if(!frame_data) {
        fprintf(stderr, "gsr error: gsr_image_writer_write_to_file: failed to allocate memory for image frame\n");
        return false;
    }

    // TODO: hdr support
    self->egl->glBindTexture(GL_TEXTURE_2D, self->texture);
    // We could use glGetTexSubImage, but it's only available starting from opengl 4.5
    self->egl->glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, frame_data);
    self->egl->glBindTexture(GL_TEXTURE_2D, 0);

    self->egl->glFlush();
    self->egl->glFinish();
    
    const bool success = gsr_image_writer_write_memory_to_file(self, filepath, image_format, quality, frame_data);
    free(frame_data);
    return success;
}

bool gsr_image_writer_write_to_file(gsr_image_writer *self, const char *filepath, gsr_image_format image_format, int quality) {
    switch(self->source) {
        case GSR_IMAGE_WRITER_SOURCE_OPENGL:
            return gsr_image_writer_write_opengl_texture_to_file(self, filepath, image_format, quality);
        case GSR_IMAGE_WRITER_SOURCE_MEMORY:
            return gsr_image_writer_write_memory_to_file(self, filepath, image_format, quality, self->memory);
    }
    return false;
}
