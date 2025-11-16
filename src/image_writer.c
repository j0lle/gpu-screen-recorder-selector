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
    self->egl = egl;
    self->width = width;
    self->height = height;
    self->texture = gl_create_texture(self->egl, self->width, self->height, GL_RGBA8, GL_RGBA, GL_NEAREST); /* TODO: use GL_RGB16 instead of GL_RGB8 for hdr/10-bit */
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

static bool gsr_image_writer_write_memory_to_file(gsr_image_writer *self, const char *filepath, gsr_image_format image_format, int quality, const void *data) {
    if(quality < 1)
        quality = 1;
    else if(quality > 100)
        quality = 100;

    bool success = false;
    switch(image_format) {
        case GSR_IMAGE_FORMAT_JPEG:
            success = stbi_write_jpg(filepath, self->width, self->height, 4, data, quality);
            break;
        case GSR_IMAGE_FORMAT_PNG:
            success = stbi_write_png(filepath, self->width, self->height, 4, data, 0);
            break;
    }

    if(!success)
        fprintf(stderr, "gsr error: gsr_image_writer_write_to_file: failed to write image data to output file %s\n", filepath);

    return success;
}

static bool gsr_image_writer_write_opengl_texture_to_file(gsr_image_writer *self, const char *filepath, gsr_image_format image_format, int quality) {
    uint8_t *frame_data = malloc(self->width * self->height * 4);
    if(!frame_data) {
        fprintf(stderr, "gsr error: gsr_image_writer_write_to_file: failed to allocate memory for image frame\n");
        return false;
    }

    unsigned int fbo = 0;
    self->egl->glGenFramebuffers(1, &fbo);
    self->egl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    self->egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->texture, 0);

    self->egl->glReadPixels(0, 0, self->width, self->height, GL_RGBA, GL_UNSIGNED_BYTE, frame_data);

    self->egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    self->egl->glDeleteFramebuffers(1, &fbo);

    self->egl->glFlush();
    self->egl->glFinish();
    
    const bool success = gsr_image_writer_write_memory_to_file(self, filepath, image_format, quality, frame_data);
    free(frame_data);
    return success;
}

bool gsr_image_writer_write_to_file(gsr_image_writer *self, const char *filepath, gsr_image_format image_format, int quality) {
    return gsr_image_writer_write_opengl_texture_to_file(self, filepath, image_format, quality);
}
