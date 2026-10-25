#include "color_engine.h"
#include "preset_manager.h"

FujiResult color_engine_init(ColorEngineContext* ctx, PresetId preset, int mode) {
    if (!ctx) return FUJI_ERR_NULL_POINTER;
    preset_manager_init();

    const PresetDefinition* def = preset_manager_get(preset);
    ctx->matrix = def->matrix;
    ctx->lut = def->lut;
    ctx->mode = mode;
    ctx->current_preset = preset;

    return FUJI_SUCCESS;
}

FujiResult color_engine_process_rgb24(ColorEngineContext* ctx, uint8_t* rgb_buffer, int width, int height) {
    if (!ctx || !rgb_buffer) return FUJI_ERR_NULL_POINTER;
    if (width <= 0 || height <= 0) return FUJI_ERR_INVALID_PARAM;

    int total_pixels = width * height;
    uint8_t* ptr = rgb_buffer;

    switch (ctx->mode) {
        case COLOR_MODE_MATRIX_ONLY:
            for (int i = 0; i < total_pixels; i++) {
                matrix_apply_rgb(&ctx->matrix, &ptr[0], &ptr[1], &ptr[2]);
                ptr += 3;
            }
            break;

        case COLOR_MODE_SAFE_FASTEST:
        case COLOR_MODE_MATRIX_LUT:
        default:
            // High-speed combined pass: Matrix then LUT
            for (int i = 0; i < total_pixels; i++) {
                matrix_apply_rgb(&ctx->matrix, &ptr[0], &ptr[1], &ptr[2]);
                lut_apply_rgb(&ctx->lut, &ptr[0], &ptr[1], &ptr[2]);
                ptr += 3;
            }
            break;
    }

    return FUJI_SUCCESS;
}

FujiResult color_engine_process_rgb565(ColorEngineContext* ctx, uint16_t* rgb565_buffer, int width, int height) {
    if (!ctx || !rgb565_buffer) return FUJI_ERR_NULL_POINTER;
    if (width <= 0 || height <= 0) return FUJI_ERR_INVALID_PARAM;

    int total_pixels = width * height;
    for (int i = 0; i < total_pixels; i++) {
        uint16_t px = rgb565_buffer[i];

        // Unpack RGB_565 -> 8-bit components
        uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);

        matrix_apply_rgb(&ctx->matrix, &r, &g, &b);
        if (ctx->mode != COLOR_MODE_MATRIX_ONLY) {
            lut_apply_rgb(&ctx->lut, &r, &g, &b);
        }

        // Repack into RGB_565
        rgb565_buffer[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    return FUJI_SUCCESS;
}

FujiResult color_engine_process_nv21(ColorEngineContext* ctx, uint8_t* nv21_buffer, int width, int height) {
    if (!ctx || !nv21_buffer) return FUJI_ERR_NULL_POINTER;
    if (width <= 0 || height <= 0) return FUJI_ERR_INVALID_PARAM;

    int total_pixels = width * height;
    uint8_t* y_plane = nv21_buffer;
    uint8_t* vu_plane = nv21_buffer + total_pixels;

    // Fast YUV approximation: Apply Green channel LUT to Luma (Y)
    for (int i = 0; i < total_pixels; i++) {
        y_plane[i] = ctx->lut.g[y_plane[i]];
    }

    // Chroma saturation scaling derived from matrix diagonal trace
    int32_t sat_factor = (ctx->matrix.m[0][0] + ctx->matrix.m[1][1] + ctx->matrix.m[2][2]) / 3;
    int uv_len = total_pixels / 2;

    for (int i = 0; i < uv_len; i += 2) {
        int32_t v = (int32_t)vu_plane[i] - 128;
        int32_t u = (int32_t)vu_plane[i + 1] - 128;

        v = (v * sat_factor) >> 8;
        u = (u * sat_factor) >> 8;

        vu_plane[i]     = clamp_u8(v + 128);
        vu_plane[i + 1] = clamp_u8(u + 128);
    }

    return FUJI_SUCCESS;
}
