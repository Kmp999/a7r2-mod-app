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
 * Precomputed 64x64 Luma-Weighted Grain Table (4096 bytes).
 * Initialized once at startup using high-frequency Xorshift PRNG.
 * Eliminates all per-pixel hash multiplications in the 10.6 MP hot loop.
 */
static int8_t STATIC_GRAIN_64x64[4096];
static int s_static_grain_initialized = 0;

static inline void init_static_grain_table(void) {
    if (s_static_grain_initialized) return;
    uint32_t state = 0x243F6A88; // Deterministic seed
    for (int i = 0; i < 4096; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        // Zero-centered pseudo-random noise between -16 and +15
        STATIC_GRAIN_64x64[i] = (int8_t)((state & 0x1F) - 16);
    }
    s_static_grain_initialized = 1;
}

// 2x2 Bayer Ordered Dithering Matrix for smooth gradients on 10.6 MP RGB565
static const int8_t BAYER_2X2[2][2] = {
    { -1,  1 },
    {  2,  0 }
};

static inline uint8_t clamp_u8_fast(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

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
    init_static_grain_table();
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
    init_static_grain_table();
}

#define LUT17_OFFSET(rr, gg, bb) (((bb) * 289 + (gg) * 17 + (rr)) * 3)

/**
 * Fast branchless fixed-point bit-shift Trilinear Interpolation for a single RGB pixel.
 * Branchless white-point fix: Remaps 0..255 to 0..256 so that 255 reaches node 16 cleanly.
 * Zero integer divisions (256/16 = 16 -> bitshift >> 4, fraction & 0x0F).
 */
static inline void lut17_interpolate_pixel(const Lut17Context* lut,
                                           uint8_t r, uint8_t g, uint8_t b,
                                           uint8_t* out_r, uint8_t* out_g, uint8_t* out_b) {
    // Map 0..255 to 0..256 so that 255 reaches index 16 exactly with zero clipping
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
 * Unpacks RGB565 to 8-bit using 1-cycle bit replication (zero divisions).
 */
static inline void unpack_rgb565_fast(uint16_t px, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t r5 = (px >> 11) & 0x1F;
    uint8_t g6 = (px >> 5)  & 0x3F;
    uint8_t b5 =  px        & 0x1F;
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/**
 * Applies the 17x17x17 3D LUT in-place to a packed RGB_565 buffer with division-free unpacking.
 */
static inline void lut17_process_rgb565(const Lut17Context* lut, uint16_t* buffer, int count) {
    int i;
    for (i = 0; i < count; i++) {
        uint8_t r, g, b;
        unpack_rgb565_fast(buffer[i], &r, &g, &b);

        uint8_t or_val, og_val, ob_val;
        lut17_interpolate_pixel(lut, r, g, b, &or_val, &og_val, &ob_val);

        buffer[i] = (uint16_t)(((or_val >> 3) << 11) | ((og_val >> 2) << 5) | (ob_val >> 3));
    }
}

/**
 * Applies 17^3 3D LUT, Precomputed 64x64 Grain, Clarity Micro-Contrast,
 * and 2x2 Bayer Ordered Dithering in a single high-speed pass.
 * Optimized for ~10.6 MP (3984 x 2656) processing in < 350 ms on dual-core ARM Cortex-A9.
 *
 * @param grain_strength   0 = Off, 1 = Weak (~8), 2 = Strong (~16)
 * @param clarity_strength -5 to +5 (0 = Neutral)
 */
static inline void lut17_process_rgb565_advanced(const Lut17Context* lut,
                                                uint16_t* buffer,
                                                int width, int height,
                                                int grain_strength,
                                                int clarity_strength) {
    if (!lut || !buffer || width <= 0 || height <= 0) return;

    init_static_grain_table();
    int grain_scale = (grain_strength == 2) ? 16 : (grain_strength == 1) ? 8 : 0;
    int idx = 0;

    for (int y = 0; y < height; y++) {
        const int8_t* bayer_row = BAYER_2X2[y & 1];
        int grain_y_offset = (y & 63) << 6;
        int prev_luma = 128;

        for (int x = 0; x < width; x++) {
            uint16_t px = buffer[idx];

            // 1. Division-free bit-replication unpacking
            uint8_t r, g, b;
            unpack_rgb565_fast(px, &r, &g, &b);

            // 2. 17x17x17 3D LUT Trilinear Interpolation
            uint8_t or_val, og_val, ob_val;
            lut17_interpolate_pixel(lut, r, g, b, &or_val, &og_val, &ob_val);

            // 3. Fast Rec.601 Luma calculation
            int luma = (or_val * 77 + og_val * 150 + ob_val * 29) >> 8;

            // 4. Precomputed 64x64 Grain Lookup with Parabolic Midtone Envelope
            int grain_delta = 0;
            if (grain_scale > 0) {
                int8_t raw_noise = STATIC_GRAIN_64x64[grain_y_offset | (x & 63)];
                int midtone_weight = (luma * (255 - luma)) >> 8; // Peaks at 128 (63), 0 at black/white
                grain_delta = (raw_noise * grain_scale * midtone_weight) >> 8;
            }

            // 5. Fast Micro-Contrast (Clarity) Boost along scanline
            int clarity_delta = 0;
            if (clarity_strength != 0) {
                int dl = luma - prev_luma;
                clarity_delta = (dl * clarity_strength) >> 4;
            }
            prev_luma = luma;

            // 6. 2x2 Bayer Ordered Dithering
            int dither = bayer_row[x & 1];

            int total_delta = grain_delta + clarity_delta + dither;
            int dr = clamp_u8_fast(or_val + total_delta);
            int dg = clamp_u8_fast(og_val + total_delta - (dither >> 1));
            int db = clamp_u8_fast(ob_val + total_delta);

            // 7. Repack to RGB565
            buffer[idx++] = (uint16_t)(((dr >> 3) << 11) | ((dg >> 2) << 5) | (db >> 3));
        }
    }
}

/**
 * Backward compatibility wrapper for grain-only pass.
 */
static inline void lut17_process_rgb565_with_grain(const Lut17Context* lut,
                                                   uint16_t* buffer,
                                                   int width, int height,
                                                   int grain_strength) {
    lut17_process_rgb565_advanced(lut, buffer, width, height, grain_strength, 0);
}

#ifdef __cplusplus
}
#endif

#endif // LUT_CUBE_17_H
