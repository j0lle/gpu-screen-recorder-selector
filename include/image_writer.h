#ifndef GSR_IMAGE_WRITER_H
#define GSR_IMAGE_WRITER_H

#include <stdbool.h>

typedef struct gsr_egl gsr_egl;

typedef enum {
    GSR_IMAGE_FORMAT_JPEG,
    GSR_IMAGE_FORMAT_PNG
} gsr_image_format;

typedef struct {
    gsr_egl *egl;
    int width;
    int height;
    unsigned int texture;
} gsr_image_writer;

bool gsr_image_writer_init_opengl(gsr_image_writer *self, gsr_egl *egl, int width, int height);
void gsr_image_writer_deinit(gsr_image_writer *self);

/* Quality is between 1 and 100 where 100 is the max quality. Quality doesn't apply to lossless formats */
bool gsr_image_writer_write_to_file(gsr_image_writer *self, const char *filepath, gsr_image_format image_format, int quality);

#endif /* GSR_IMAGE_WRITER_H */
