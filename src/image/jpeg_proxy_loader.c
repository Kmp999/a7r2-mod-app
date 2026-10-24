#include "jpeg_proxy_loader.h"

FujiResult jpeg_parse_header_fast(const uint8_t* data, size_t size, JpegHeaderInfo* out_info) {
    if (!data || !out_info || size < 4) return FUJI_ERR_NULL_POINTER;
    out_info->is_valid_jpeg = 0;
    out_info->width = 0;
    out_info->height = 0;

    // Check SOI marker 0xFFD8
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return FUJI_ERR_INVALID_PARAM;
    }

    size_t pos = 2;
    while (pos + 4 < size) {
        if (data[pos] != 0xFF) {
            pos++;
            continue;
        }
        uint8_t marker = data[pos + 1];
        if (marker == 0xC0 || marker == 0xC2) { // SOF0 or SOF2
            out_info->height = (data[pos + 5] << 8) | data[pos + 6];
            out_info->width  = (data[pos + 7] << 8) | data[pos + 8];
            out_info->is_valid_jpeg = 1;
            return FUJI_SUCCESS;
        }
        uint16_t len = (data[pos + 2] << 8) | data[pos + 3];
        pos += 2 + len;
    }

    return FUJI_ERR_INVALID_PARAM;
}
