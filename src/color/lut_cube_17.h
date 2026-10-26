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

/**
 * Pre-computes 17x17x17 3D LUT starting from a base film simulation table (e.g. Kodak 2383, Fuji 3510, etc.)
 * and overlays custom recipe parameters (Color Correction Matrix + 1D Tone curves).
 */
static inline void lut17_precompute_with_base(Lut17Context* lut,
                                              const uint8_t* base_table,
                                              const int16_t ccm_q8[9],
                                              const uint8_t tone_curve_r[256],
                                              const uint8_t tone_curve_g[256],
                                              const uint8_t tone_curve_b[256]) {
    if (base_table == NULL) {
        lut17_precompute(lut, ccm_q8, tone_curve_r, tone_curve_g, tone_curve_b);
        return;
    }
    int idx = 0;
    int i;
    for (i = 0; i < LUT_TOTAL_NODES; i++) {
        int r_val = base_table[idx];
        int g_val = base_table[idx + 1];
        int b_val = base_table[idx + 2];

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

#define LUT17_OFFSET(rr, gg, bb) (((bb) * 289 + (gg) * 17 + (rr)) * 3)

/**
 * Fast fixed-point bit-shift Trilinear Interpolation for a single RGB pixel.
 * Takes 0 division operations (256/16 = 16 -> bitshift >> 4, fraction & 0x0F).
 */
static inline void lut17_interpolate_pixel(const Lut17Context* lut,
                                           uint8_t r, uint8_t g, uint8_t b,
                                           uint8_t* out_r, uint8_t* out_g, uint8_t* out_b) {
    // Map 0..255 to 0..256 so that 255 reaches index 16 exactly
    int r256 = r + (r >> 7);
    int g256 = g + (g >> 7);
    int b256 = b + (b >> 7);

    int r0 = r256 >> 4;
    int g0 = g256 >> 4;
    int b0 = b256 >> 4;

    int r_frac = r256 & 0x0F;
    int g_frac = g256 & 0x0F;
    int b_frac = b256 & 0x0F;

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

// ----------------------------------------------------------------------------
// Stage 2B: Fast Native Spatial Grain Pass (64x64 Tileable Midtone-Weighted)
// ----------------------------------------------------------------------------
#define GRAIN_TILE_DIM 64
#define GRAIN_TILE_MASK 63

// Fast LCG pseudo-random 64x64 tileable noise table
static inline int8_t get_grain_noise(int x, int y) {
    // Spatial hash for deterministic 64x64 repeat with zero storage footprint
    uint32_t h = (uint32_t)((x & GRAIN_TILE_MASK) * 73856093 ^ (y & GRAIN_TILE_MASK) * 19349663);
    h = (h ^ (h >> 13)) * 1664525;
    return (int8_t)((h >> 16) & 0x1F) - 16; // -16 to +15 centered noise
}

static inline uint8_t clamp_u8_fast(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/**
 * Applies 17^3 3D LUT and 64x64 midtone-modulated Grain in a single high-speed pass.
 * @param grain_strength 0 = Off, 1 = Weak (~8), 2 = Strong (~16)
 */
static inline void lut17_process_rgb565_with_grain(const Lut17Context* lut,
                                                   uint16_t* buffer,
                                                   int width, int height,
                                                   int grain_strength) {
    if (!lut || !buffer || width <= 0 || height <= 0) return;

    int scale = (grain_strength == 2) ? 16 : (grain_strength == 1) ? 8 : 0;
    int idx = 0;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t px = buffer[idx];

            uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);

            uint8_t or_val, og_val, ob_val;
            lut17_interpolate_pixel(lut, r, g, b, &or_val, &og_val, &ob_val);

            if (scale > 0) {
                // Approximate Rec.601 luma: (R*77 + G*150 + B*29) >> 8
                int luma = (or_val * 77 + og_val * 150 + ob_val * 29) >> 8;
                // Parabolic midtone weight envelope: peaks at mid-grey 128, rolls off at 0 and 255
                int midtone_weight = (luma * (255 - luma)) >> 8; // 0..63
                int8_t raw_noise = get_grain_noise(x, y);
                int delta = (raw_noise * scale * midtone_weight) >> 8;

                or_val = clamp_u8_fast(or_val + delta);
                og_val = clamp_u8_fast(og_val + delta);
                ob_val = clamp_u8_fast(ob_val + delta);
            }

            buffer[idx++] = (uint16_t)(((or_val >> 3) << 11) | ((og_val >> 2) << 5) | (ob_val >> 3));
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif // LUT_CUBE_17_H
