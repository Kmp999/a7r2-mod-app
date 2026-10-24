#ifndef FUJI_IMAGE_BUFFER_H
#define FUJI_IMAGE_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include "../util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_FMT_RGB24 = 0,
    IMG_FMT_RGBA32,
    IMG_FMT_NV21,
    IMG_FMT_JPEG_STREAM
} ImageFormat;

typedef struct {
    uint8_t* data;
    size_t size_bytes;
    int width;
    int height;
    int stride;
    ImageFormat format;
} ImageBuffer;

FujiResult image_buffer_init(ImageBuffer* buf, uint8_t* memory, size_t size_bytes, int width, int height, ImageFormat fmt);

#ifdef __cplusplus
}
#endif

#endif // FUJI_IMAGE_BUFFER_H
