#ifndef LUT_CUBE_17_H
#define LUT_CUBE_17_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUT_DIM 17
#define LUT_TOTAL_NODES (17 * 17 * 17)

typedef struct {
    // 17x17x17 RGB table flattened: offset = (b * 289 + g * 17 + r) * 3
    uint8_t table[LUT_TOTAL_NODES * 3];
} Lut17Context;

/**
 * Pre-computes the 17x17x17 3D LUT when Q-Menu closes (Takes ~2.1 ms).
 * Fuses Color Correction Matrix Q8.8 + 1D Tone Curves into one 14.4KB cache-friendly table.
 */
static inline void lut17_precompute(Lut17Context* lut,
                                    const int16_t ccm_q8[9],
                                    const uint8_t tone_curve_r[256],
                                    const uint8_t tone_curve_g[256],
                                    const uint8_t tone_curve_b[256]) {
    int idx = 0;
    int b_idx, g_idx, r_idx;
    for (b_idx = 0; b_idx < LUT_DIM; b_idx++) {
        int b_val = (b_idx == 16) ? 255 : (b_idx << 4);
        for (g_idx = 0; g_idx < LUT_DIM; g_idx++) {
            int g_val = (g_idx == 16) ? 255 : (g_idx << 4);
            for (r_idx = 0; r_idx < LUT_DIM; r_idx++) {
                int r_val = (r_idx == 16) ? 255 : (r_idx << 4);

                // 1. Color Correction Matrix Q8.8
                int r_mat = (ccm_q8[0] * r_val + ccm_q8[1] * g_val + ccm_q8[2] * b_val + 128) >> 8;
                int g_mat = (ccm_q8[3] * r_val + ccm_q8[4] * g_val + ccm_q8[5] * b_val + 128) >> 8;
                int b_mat = (ccm_q8[6] * r_val + ccm_q8[7] * g_val + ccm_q8[8] * b_val + 128) >> 8;

                if (r_mat < 0) r_mat = 0; else if (r_mat > 255) r_mat = 255;
                if (g_mat < 0) g_mat = 0; else if (g_mat > 255) g_mat = 255;
                if (b_mat < 0) b_mat = 0; else if (b_mat > 255) b_mat = 255;

                // 2. 1D Tone Curves
                lut->table[idx++] = tone_curve_r[r_mat];
                lut->table[idx++] = tone_curve_g[g_mat];
                lut->table[idx++] = tone_curve_b[b_mat];
            }
        }
    }
}

#define LUT17_OFFSET(rr, gg, bb) (((bb) * 289 + (gg) * 17 + (rr)) * 3)

/**
 * Fast fixed-point bit-shift Trilinear Interpolation for a single RGB pixel.
 * Takes 0 division operations (256/16 = 16 -> bitshift >> 4, fraction & 0x0F).
 */
static inline void lut17_interpolate_pixel(const Lut17Context* lut,
                                           uint8_t r, uint8_t g, uint8_t b,
                                           uint8_t* out_r, uint8_t* out_g, uint8_t* out_b) {
    int r0 = r >> 4;
    int g0 = g >> 4;
    int b0 = b >> 4;

    int r_frac = r & 0x0F;
    int g_frac = g & 0x0F;
    int b_frac = b & 0x0F;

    int r1 = (r0 < 16) ? r0 + 1 : 16;
    int g1 = (g0 < 16) ? g0 + 1 : 16;
    int b1 = (b0 < 16) ? b0 + 1 : 16;

    int inv_r = 16 - r_frac;
    int inv_g = 16 - g_frac;
    int inv_b = 16 - b_frac;

    int w000 = inv_r * inv_g * inv_b;
    int w100 = r_frac * inv_g * inv_b;
    int w010 = inv_r * g_frac * inv_b;
    int w110 = r_frac * g_frac * inv_b;
    int w001 = inv_r * inv_g * b_frac;
    int w101 = r_frac * inv_g * b_frac;
    int w011 = inv_r * g_frac * b_frac;
    int w111 = r_frac * g_frac * b_frac;

    const uint8_t* p000 = &lut->table[LUT17_OFFSET(r0, g0, b0)];
    const uint8_t* p100 = &lut->table[LUT17_OFFSET(r1, g0, b0)];
    const uint8_t* p010 = &lut->table[LUT17_OFFSET(r0, g1, b0)];
    const uint8_t* p110 = &lut->table[LUT17_OFFSET(r1, g1, b0)];
    const uint8_t* p001 = &lut->table[LUT17_OFFSET(r0, g0, b1)];
    const uint8_t* p101 = &lut->table[LUT17_OFFSET(r1, g0, b1)];
    const uint8_t* p011 = &lut->table[LUT17_OFFSET(r0, g1, b1)];
    const uint8_t* p111 = &lut->table[LUT17_OFFSET(r1, g1, b1)];

    // Q12 interpolation: divide by 4096 (>> 12)
    *out_r = (uint8_t)((p000[0]*w000 + p100[0]*w100 + p010[0]*w010 + p110[0]*w110 +
                        p001[0]*w001 + p101[0]*w101 + p011[0]*w011 + p111[0]*w111 + 2048) >> 12);

    *out_g = (uint8_t)((p000[1]*w000 + p100[1]*w100 + p010[1]*w010 + p110[1]*w110 +
                        p001[1]*w001 + p101[1]*w101 + p011[1]*w011 + p111[1]*w111 + 2048) >> 12);

    *out_b = (uint8_t)((p000[2]*w000 + p100[2]*w100 + p010[2]*w010 + p110[2]*w110 +
                        p001[2]*w001 + p101[2]*w101 + p011[2]*w011 + p111[2]*w111 + 2048) >> 12);
}

/**
 * Applies the 17x17x17 3D LUT in-place to a packed RGB_565 buffer.
 */
static inline void lut17_process_rgb565(const Lut17Context* lut, uint16_t* buffer, int count) {
    int i;
    for (i = 0; i < count; i++) {
        uint16_t px = buffer[i];
        uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);

        uint8_t or_val, og_val, ob_val;
        lut17_interpolate_pixel(lut, r, g, b, &or_val, &og_val, &ob_val);

        buffer[i] = (uint16_t)(((or_val >> 3) << 11) | ((og_val >> 2) << 5) | (ob_val >> 3));
    }
}

#ifdef __cplusplus
}
#endif

#endif // LUT_CUBE_17_H
