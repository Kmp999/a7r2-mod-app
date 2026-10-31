#ifndef FUJI_FIXED_POINT_MATRIX_H
#define FUJI_FIXED_POINT_MATRIX_H

#include <stdint.h>

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Q8.8 fixed-point arithmetic helpers
// 1.0f in Q8.8 is 256
typedef int16_t q8_8_t;

#define TO_Q8_8(val) ((q8_8_t)((val) * 256.0f))
#define FROM_Q8_8(val) (((float)(val)) / 256.0f)

typedef struct {
    q8_8_t m[3][3];
} Matrix3x3;

// Clamps value strictly to [0, 255]
static inline uint8_t clamp_u8(int32_t val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

// In-place 3x3 matrix multiplication on RGB pixel using pure integer arithmetic
static inline void matrix_apply_rgb(const Matrix3x3* __restrict mat, uint8_t* __restrict r, uint8_t* __restrict g, uint8_t* __restrict b) {
    int32_t rin = *r;
    int32_t gin = *g;
    int32_t bin = *b;

    int32_t rout = (mat->m[0][0] * rin + mat->m[0][1] * gin + mat->m[0][2] * bin + 128) >> 8;
    int32_t gout = (mat->m[1][0] * rin + mat->m[1][1] * gin + mat->m[1][2] * bin + 128) >> 8;
    int32_t bout = (mat->m[2][0] * rin + mat->m[2][1] * gin + mat->m[2][2] * bin + 128) >> 8;

    *r = clamp_u8(rout);
    *g = clamp_u8(gout);
    *b = clamp_u8(bout);
}

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
/**
 * ARMv7 NEON SIMD Vectorized CCM Q8 Matrix Transformation.
 * Processes 8 pixels in parallel using vmlal_n_s16 and vqmovun_s16 for 1-cycle saturation.
 */
static inline void matrix_apply_rgb_neon8(const Matrix3x3* __restrict mat,
                                         uint8_t* __restrict r_ptr,
                                         uint8_t* __restrict g_ptr,
                                         uint8_t* __restrict b_ptr) {
    uint8x8_t r8 = vld1_u8(r_ptr);
    uint8x8_t g8 = vld1_u8(g_ptr);
    uint8x8_t b8 = vld1_u8(b_ptr);

    int16x8_t r16 = vreinterpretq_s16_u16(vmovl_u8(r8));
    int16x8_t g16 = vreinterpretq_s16_u16(vmovl_u8(g8));
    int16x8_t b16 = vreinterpretq_s16_u16(vmovl_u8(b8));

    // Channel 0 (R)
    int32x4_t r_lo = vdupq_n_s32(128);
    int32x4_t r_hi = vdupq_n_s32(128);
    r_lo = vmlal_n_s16(r_lo, vget_low_s16(r16), mat->m[0][0]);
    r_hi = vmlal_n_s16(r_hi, vget_high_s16(r16), mat->m[0][0]);
    r_lo = vmlal_n_s16(r_lo, vget_low_s16(g16), mat->m[0][1]);
    r_hi = vmlal_n_s16(r_hi, vget_high_s16(g16), mat->m[0][1]);
    r_lo = vmlal_n_s16(r_lo, vget_low_s16(b16), mat->m[0][2]);
    r_hi = vmlal_n_s16(r_hi, vget_high_s16(b16), mat->m[0][2]);
    int16x8_t r_out16 = vcombine_s16(vshrn_n_s32(r_lo, 8), vshrn_n_s32(r_hi, 8));
    uint8x8_t r_res = vqmovun_s16(r_out16);

    // Channel 1 (G)
    int32x4_t g_lo = vdupq_n_s32(128);
    int32x4_t g_hi = vdupq_n_s32(128);
    g_lo = vmlal_n_s16(g_lo, vget_low_s16(r16), mat->m[1][0]);
    g_hi = vmlal_n_s16(g_hi, vget_high_s16(r16), mat->m[1][0]);
    g_lo = vmlal_n_s16(g_lo, vget_low_s16(g16), mat->m[1][1]);
    g_hi = vmlal_n_s16(g_hi, vget_high_s16(g16), mat->m[1][1]);
    g_lo = vmlal_n_s16(g_lo, vget_low_s16(b16), mat->m[1][2]);
    g_hi = vmlal_n_s16(g_hi, vget_high_s16(b16), mat->m[1][2]);
    int16x8_t g_out16 = vcombine_s16(vshrn_n_s32(g_lo, 8), vshrn_n_s32(g_hi, 8));
    uint8x8_t g_res = vqmovun_s16(g_out16);

    // Channel 2 (B)
    int32x4_t b_lo = vdupq_n_s32(128);
    int32x4_t b_hi = vdupq_n_s32(128);
    b_lo = vmlal_n_s16(b_lo, vget_low_s16(r16), mat->m[2][0]);
    b_hi = vmlal_n_s16(b_hi, vget_high_s16(r16), mat->m[2][0]);
    b_lo = vmlal_n_s16(b_lo, vget_low_s16(g16), mat->m[2][1]);
    b_hi = vmlal_n_s16(b_hi, vget_high_s16(g16), mat->m[2][1]);
    b_lo = vmlal_n_s16(b_lo, vget_low_s16(b16), mat->m[2][2]);
    b_hi = vmlal_n_s16(b_hi, vget_high_s16(b16), mat->m[2][2]);
    int16x8_t b_out16 = vcombine_s16(vshrn_n_s32(b_lo, 8), vshrn_n_s32(b_hi, 8));
    uint8x8_t b_res = vqmovun_s16(b_out16);

    vst1_u8(r_ptr, r_res);
    vst1_u8(g_ptr, g_res);
    vst1_u8(b_ptr, b_res);
}
#endif

#ifdef __cplusplus
}
#endif

#endif // FUJI_FIXED_POINT_MATRIX_H
