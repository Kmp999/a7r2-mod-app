#ifndef FUJI_JPEG_PROXY_LOADER_H
#define FUJI_JPEG_PROXY_LOADER_H

#include "image_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fast JPEG Proxy header parser to obtain dimensions and subsampling
typedef struct {
    int width;
    int height;
    int is_valid_jpeg;
} JpegHeaderInfo;

FujiResult jpeg_parse_header_fast(const uint8_t* jpeg_data, size_t size, JpegHeaderInfo* out_info);

#ifdef __cplusplus
}
#endif

#endif // FUJI_JPEG_PROXY_LOADER_H
