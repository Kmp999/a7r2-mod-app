#include "downscaler.h"

FujiResult downscale_rgb24_nearest(const ImageBuffer* src, ImageBuffer* dst) {
    if (!src || !dst || !src->data || !dst->data) return FUJI_ERR_NULL_POINTER;
    if (src->width <= 0 || src->height <= 0 || dst->width <= 0 || dst->height <= 0) return FUJI_ERR_INVALID_PARAM;

    int sw = src->width;
    int sh = src->height;
    int dw = dst->width;
    int dh = dst->height;

    // Fixed point step in Q16: 65536
    uint32_t step_x = ((uint32_t)sw << 16) / (uint32_t)dw;
    uint32_t step_y = ((uint32_t)sh << 16) / (uint32_t)dh;

    const uint8_t* src_data = src->data;
    uint8_t* dst_data = dst->data;

    for (int y = 0; y < dh; y++) {
        uint32_t sy = (y * step_y) >> 16;
        const uint8_t* src_row = src_data + (sy * sw * 3);
        uint8_t* dst_row = dst_data + (y * dw * 3);

        for (int x = 0; x < dw; x++) {
            uint32_t sx = (x * step_x) >> 16;
            const uint8_t* sp = src_row + (sx * 3);
            uint8_t* dp = dst_row + (x * 3);

            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
        }
    }

    return FUJI_SUCCESS;
}

FujiResult downscale_nv21_nearest(const ImageBuffer* src, ImageBuffer* dst) {
    if (!src || !dst || !src->data || !dst->data) return FUJI_ERR_NULL_POINTER;
    int sw = src->width;
    int sh = src->height;
    int dw = dst->width;
    int dh = dst->height;

    uint32_t step_x = ((uint32_t)sw << 16) / (uint32_t)dw;
    uint32_t step_y = ((uint32_t)sh << 16) / (uint32_t)dh;

    const uint8_t* src_y = src->data;
    uint8_t* dst_y = dst->data;

    // Downscale Y plane
    for (int y = 0; y < dh; y++) {
        uint32_t sy = (y * step_y) >> 16;
        const uint8_t* s_row = src_y + (sy * sw);
        uint8_t* d_row = dst_y + (y * dw);

        for (int x = 0; x < dw; x++) {
            uint32_t sx = (x * step_x) >> 16;
            d_row[x] = s_row[sx];
        }
    }

    // Downscale VU plane
    const uint8_t* src_vu = src->data + (sw * sh);
    uint8_t* dst_vu = dst->data + (dw * dh);
    int dh_uv = dh / 2;
    int dw_uv = dw / 2;

    for (int y = 0; y < dh_uv; y++) {
        uint32_t sy = (y * step_y) >> 16;
        const uint8_t* s_row = src_vu + (sy * sw);
        uint8_t* d_row = dst_vu + (y * dw);

        for (int x = 0; x < dw_uv; x++) {
            uint32_t sx = (x * step_x) >> 16;
            // VU pair
            d_row[x * 2]     = s_row[sx * 2];
            d_row[x * 2 + 1] = s_row[sx * 2 + 1];
        }
    }

    return FUJI_SUCCESS;
}
