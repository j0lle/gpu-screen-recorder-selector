#include "../include/image_writer.h"
#include "../include/egl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../external/stb_image_write.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

static unsigned int gl_create_texture(gsr_egl *egl, int width, int height, int internal_format, unsigned int format) {
    unsigned int texture_id = 0;
    egl->glGenTextures(1, &texture_id);
    egl->glBindTexture(GL_TEXTURE_2D, texture_id);
    egl->glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, NULL);

    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    egl->glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}

/* TODO: Support hdr/10-bit */
bool gsr_image_writer_init(gsr_image_writer *self, gsr_image_writer_source source, gsr_egl *egl, int width, int height) {
    assert(source == GSR_IMAGE_WRITER_SOURCE_OPENGL);
    self->source = source;
    self->egl = egl;
    self->width = width;
    self->height = height;
    self->texture = gl_create_texture(self->egl, self->width, self->height, GL_RGB8, GL_RGB); /* TODO: use GL_RGB16 instead of GL_RGB8 for hdr/10-bit */
    if(self->texture == 0) {
        fprintf(stderr, "gsr error: gsr_image_writer_init: failed to create texture\n");
        return false;
    }
    return true;
}

void gsr_image_writer_deinit(gsr_image_writer *self) {
    if(self->texture) {
        self->egl->glDeleteTextures(1, &self->texture);
        self->texture = 0;
    }
}

bool gsr_image_writer_write_to_file(gsr_image_writer *self, const char *filepath, gsr_image_format image_format, int quality) {
    if(quality < 1)
        quality = 1;
    else if(quality > 100)
        quality = 100;

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
    
    bool success = false;
    switch(image_format) {
        case GSR_IMAGE_FORMAT_JPEG:
            success = stbi_write_jpg(filepath, self->width, self->height, 3, frame_data, quality);
            break;
        case GSR_IMAGE_FORMAT_PNG:
            success = stbi_write_png(filepath, self->width, self->height, 3, frame_data, 0);
            break;
    }

    if(!success)
        fprintf(stderr, "gsr error: gsr_image_writer_write_to_file: failed to write image data to output file %s\n", filepath);

    free(frame_data);
    return success;
}
