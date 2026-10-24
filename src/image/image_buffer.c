#include "image_buffer.h"

FujiResult image_buffer_init(ImageBuffer* buf, uint8_t* memory, size_t size_bytes, int width, int height, ImageFormat fmt) {
    if (!buf || !memory) return FUJI_ERR_NULL_POINTER;
    if (width <= 0 || height <= 0) return FUJI_ERR_INVALID_PARAM;

    buf->data = memory;
    buf->size_bytes = size_bytes;
    buf->width = width;
    buf->height = height;
    buf->format = fmt;

    switch (fmt) {
        case IMG_FMT_RGB24:
            buf->stride = width * 3;
            break;
        case IMG_FMT_RGBA32:
            buf->stride = width * 4;
            break;
        case IMG_FMT_NV21:
            buf->stride = width;
            break;
        case IMG_FMT_JPEG_STREAM:
        default:
            buf->stride = 0;
            break;
    }

    return FUJI_SUCCESS;
}
