#include "neon_jpeg_kernels.h"
#include <string.h>

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

static const uint8_t NEON_ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5, 12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51, 58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

// Exact 8x8 int16x8_t matrix transpose (12 NEON instructions)
#define NEON_TRANSPOSE_8x8_S16(r0, r1, r2, r3, r4, r5, r6, r7) do { \
    int16x8x2_t _a0 = vtrnq_s16((r0), (r1)); \
    int16x8x2_t _a1 = vtrnq_s16((r2), (r3)); \
    int16x8x2_t _a2 = vtrnq_s16((r4), (r5)); \
    int16x8x2_t _a3 = vtrnq_s16((r6), (r7)); \
    int32x4x2_t _b0 = vtrnq_s32(vreinterpretq_s32_s16(_a0.val[0]), vreinterpretq_s32_s16(_a1.val[0])); \
    int32x4x2_t _b1 = vtrnq_s32(vreinterpretq_s32_s16(_a0.val[1]), vreinterpretq_s32_s16(_a1.val[1])); \
    int32x4x2_t _b2 = vtrnq_s32(vreinterpretq_s32_s16(_a2.val[0]), vreinterpretq_s32_s16(_a3.val[0])); \
    int32x4x2_t _b3 = vtrnq_s32(vreinterpretq_s32_s16(_a2.val[1]), vreinterpretq_s32_s16(_a3.val[1])); \
    (r0) = vcombine_s16(vget_low_s16(vreinterpretq_s16_s32(_b0.val[0])),  vget_low_s16(vreinterpretq_s16_s32(_b2.val[0]))); \
    (r4) = vcombine_s16(vget_high_s16(vreinterpretq_s16_s32(_b0.val[0])), vget_high_s16(vreinterpretq_s16_s32(_b2.val[0]))); \
    (r2) = vcombine_s16(vget_low_s16(vreinterpretq_s16_s32(_b0.val[1])),  vget_low_s16(vreinterpretq_s16_s32(_b2.val[1]))); \
    (r6) = vcombine_s16(vget_high_s16(vreinterpretq_s16_s32(_b0.val[1])), vget_high_s16(vreinterpretq_s16_s32(_b2.val[1]))); \
    (r1) = vcombine_s16(vget_low_s16(vreinterpretq_s16_s32(_b1.val[0])),  vget_low_s16(vreinterpretq_s16_s32(_b3.val[0]))); \
    (r5) = vcombine_s16(vget_high_s16(vreinterpretq_s16_s32(_b1.val[0])), vget_high_s16(vreinterpretq_s16_s32(_b3.val[0]))); \
    (r3) = vcombine_s16(vget_low_s16(vreinterpretq_s16_s32(_b1.val[1])),  vget_low_s16(vreinterpretq_s16_s32(_b3.val[1]))); \
    (r7) = vcombine_s16(vget_high_s16(vreinterpretq_s16_s32(_b1.val[1])), vget_high_s16(vreinterpretq_s16_s32(_b3.val[1]))); \
} while (0)

// 8-wide 1D AAN IDCT butterfly (operates on 8 vectors simultaneously using Q15 SQRDMULH)
#define NEON_IDCT_1D(x0, x1, x2, x3, x4, x5, x6, x7) do { \
    int16x8_t _t10 = vaddq_s16((x0), (x4)); \
    int16x8_t _t11 = vsubq_s16((x0), (x4)); \
    int16x8_t _t13 = vaddq_s16((x2), (x6)); \
    int16x8_t _d13 = vsubq_s16((x2), (x6)); \
    int16x8_t _t12 = vsubq_s16(vaddq_s16(_d13, vqrdmulhq_n_s16(_d13, 13573)), _t13); \
    int16x8_t _t0  = vaddq_s16(_t10, _t13); \
    int16x8_t _t3  = vsubq_s16(_t10, _t13); \
    int16x8_t _t1  = vaddq_s16(_t11, _t12); \
    int16x8_t _t2  = vsubq_s16(_t11, _t12); \
    int16x8_t _z13 = vaddq_s16((x5), (x3)); \
    int16x8_t _z10 = vsubq_s16((x5), (x3)); \
    int16x8_t _z11 = vaddq_s16((x1), (x7)); \
    int16x8_t _z12 = vsubq_s16((x1), (x7)); \
    int16x8_t _t7  = vaddq_s16(_z11, _z13); \
    int16x8_t _do  = vsubq_s16(_z11, _z13); \
    int16x8_t _o11 = vaddq_s16(_do, vqrdmulhq_n_s16(_do, 13573)); \
    int16x8_t _sz  = vaddq_s16(_z10, _z12); \
    int16x8_t _z5  = vaddq_s16(_sz, vqrdmulhq_n_s16(_sz, 27779)); \
    int16x8_t _o10 = vsubq_s16(vaddq_s16(_z12, vqrdmulhq_n_s16(_z12, 2700)), _z5); \
    int16x8_t _o12 = vsubq_s16(_z5, vaddq_s16(vaddq_s16(_z10, _z10), vqrdmulhq_n_s16(_z10, 20091))); \
    int16x8_t _t6  = vsubq_s16(_o12, _t7); \
    int16x8_t _t5  = vsubq_s16(_o11, _t6); \
    int16x8_t _t4  = vaddq_s16(_o10, _t5); \
    (x0) = vaddq_s16(_t0, _t7); (x7) = vsubq_s16(_t0, _t7); \
    (x1) = vaddq_s16(_t1, _t6); (x6) = vsubq_s16(_t1, _t6); \
    (x2) = vaddq_s16(_t2, _t5); (x5) = vsubq_s16(_t2, _t5); \
    (x3) = vsubq_s16(_t3, _t4); (x4) = vaddq_s16(_t3, _t4); \
} while (0)

void neon_idct_8x8(const int16_t* in, uint8_t has_ac, const int16_t* dq_q2, const uint16_t* q_raw, uint8_t* out) {
    if (!has_ac) {
        int v = (((int)in[0] * (int)q_raw[0] + 4) >> 3) + 128;
        uint8_t c = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        memset(out, c, 64);
        return;
    }

    int16x8_t r0 = vmulq_s16(vld1q_s16(in + 0),  vld1q_s16(dq_q2 + 0));
    int16x8_t r1 = vmulq_s16(vld1q_s16(in + 8),  vld1q_s16(dq_q2 + 8));
    int16x8_t r2 = vmulq_s16(vld1q_s16(in + 16), vld1q_s16(dq_q2 + 16));
    int16x8_t r3 = vmulq_s16(vld1q_s16(in + 24), vld1q_s16(dq_q2 + 24));
    int16x8_t r4 = vmulq_s16(vld1q_s16(in + 32), vld1q_s16(dq_q2 + 32));
    int16x8_t r5 = vmulq_s16(vld1q_s16(in + 40), vld1q_s16(dq_q2 + 40));
    int16x8_t r6 = vmulq_s16(vld1q_s16(in + 48), vld1q_s16(dq_q2 + 48));
    int16x8_t r7 = vmulq_s16(vld1q_s16(in + 56), vld1q_s16(dq_q2 + 56));

    // Row pass (transpose so lanes are rows, run 8-wide 1D IDCT, >> 1 for headroom, transpose back)
    NEON_TRANSPOSE_8x8_S16(r0, r1, r2, r3, r4, r5, r6, r7);
    NEON_IDCT_1D(r0, r1, r2, r3, r4, r5, r6, r7);
    r0 = vrshrq_n_s16(r0, 1); r1 = vrshrq_n_s16(r1, 1);
    r2 = vrshrq_n_s16(r2, 1); r3 = vrshrq_n_s16(r3, 1);
    r4 = vrshrq_n_s16(r4, 1); r5 = vrshrq_n_s16(r5, 1);
    r6 = vrshrq_n_s16(r6, 1); r7 = vrshrq_n_s16(r7, 1);
    NEON_TRANSPOSE_8x8_S16(r0, r1, r2, r3, r4, r5, r6, r7);

    // Column pass across all 8 columns simultaneously -> >> 4 -> +128 -> saturating u8 pack
    NEON_IDCT_1D(r0, r1, r2, r3, r4, r5, r6, r7);
    int16x8_t c128 = vdupq_n_s16(128);
    vst1_u8(out + 0,  vqmovun_s16(vaddq_s16(vrshrq_n_s16(r0, 4), c128)));
    vst1_u8(out + 8,  vqmovun_s16(vaddq_s16(vrshrq_n_s16(r1, 4), c128)));
    vst1_u8(out + 16, vqmovun_s16(vaddq_s16(vrshrq_n_s16(r2, 4), c128)));
    vst1_u8(out + 24, vqmovun_s16(vaddq_s16(vrshrq_n_s16(r3, 4), c128)));
    vst1_u8(out + 32, vqmovun_s16(vaddq_s16(vrshrq_n_s16(r4, 4), c128)));
    vst1_u8(out + 40, vqmovun_s16(vaddq_s16(vrshrq_n_s16(r5, 4), c128)));
    vst1_u8(out + 48, vqmovun_s16(vaddq_s16(vrshrq_n_s16(r6, 4), c128)));
    vst1_u8(out + 56, vqmovun_s16(vaddq_s16(vrshrq_n_s16(r7, 4), c128)));
}

void neon_idct_8x4_half_v(const int16_t* in, uint8_t has_ac, const int16_t* dq_q2, const uint16_t* q_raw, uint8_t* out32) {
    if (!has_ac) {
        int v = (((int)in[0] * (int)q_raw[0] + 4) >> 3) + 128;
        uint8_t c = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        memset(out32, c, 32);
        return;
    }

    int16x8_t r0 = vmulq_s16(vld1q_s16(in + 0),  vld1q_s16(dq_q2 + 0));
    int16x8_t r1 = vmulq_s16(vld1q_s16(in + 8),  vld1q_s16(dq_q2 + 8));
    int16x8_t r2 = vmulq_s16(vld1q_s16(in + 16), vld1q_s16(dq_q2 + 16));
    int16x8_t r3 = vmulq_s16(vld1q_s16(in + 24), vld1q_s16(dq_q2 + 24));
    int16x8_t r4 = vdupq_n_s16(0); // Row 4 cancels identically across adjacent pairs!
    int16x8_t r5 = vmulq_s16(vld1q_s16(in + 40), vld1q_s16(dq_q2 + 40));
    int16x8_t r6 = vmulq_s16(vld1q_s16(in + 48), vld1q_s16(dq_q2 + 48));
    int16x8_t r7 = vmulq_s16(vld1q_s16(in + 56), vld1q_s16(dq_q2 + 56));

    NEON_TRANSPOSE_8x8_S16(r0, r1, r2, r3, r4, r5, r6, r7);
    NEON_IDCT_1D(r0, r1, r2, r3, r4, r5, r6, r7);
    r0 = vrshrq_n_s16(r0, 1); r1 = vrshrq_n_s16(r1, 1);
    r2 = vrshrq_n_s16(r2, 1); r3 = vrshrq_n_s16(r3, 1);
    r4 = vrshrq_n_s16(r4, 1); r5 = vrshrq_n_s16(r5, 1);
    r6 = vrshrq_n_s16(r6, 1); r7 = vrshrq_n_s16(r7, 1);
    NEON_TRANSPOSE_8x8_S16(r0, r1, r2, r3, r4, r5, r6, r7);

    // Exact 4-output half-height vertical column butterfly (only 4 NEON multiplies!)
    int16x8_t e0  = vaddq_s16(r0, r0);
    int16x8_t d26 = vsubq_s16(r2, r6);
    int16x8_t ed  = vaddq_s16(d26, vqrdmulhq_n_s16(d26, 13573));
    int16x8_t z10 = vsubq_s16(r5, r3);
    int16x8_t z12 = vsubq_s16(r1, r7);
    int16x8_t sz  = vaddq_s16(z10, z12);
    int16x8_t z5  = vaddq_s16(sz, vqrdmulhq_n_s16(sz, 27779));
    int16x8_t t10 = vsubq_s16(vaddq_s16(z12, vqrdmulhq_n_s16(z12, 2700)), z5);
    int16x8_t t12 = vsubq_s16(z5, vaddq_s16(vaddq_s16(z10, z10), vqrdmulhq_n_s16(z10, 20091)));

    int16x8_t ehi = vaddq_s16(e0, ed);
    int16x8_t elo = vsubq_s16(e0, ed);
    int16x8_t c128 = vdupq_n_s16(128);
    vst1_u8(out32 + 0,  vqmovun_s16(vaddq_s16(vrshrq_n_s16(vaddq_s16(ehi, t12), 5), c128)));
    vst1_u8(out32 + 8,  vqmovun_s16(vaddq_s16(vrshrq_n_s16(vsubq_s16(elo, t10), 5), c128)));
    vst1_u8(out32 + 16, vqmovun_s16(vaddq_s16(vrshrq_n_s16(vaddq_s16(elo, t10), 5), c128)));
    vst1_u8(out32 + 24, vqmovun_s16(vaddq_s16(vrshrq_n_s16(vsubq_s16(ehi, t12), 5), c128)));
}

// 8-wide 1D AAN FDCT butterfly
#define NEON_FDCT_1D(x0, x1, x2, x3, x4, x5, x6, x7) do { \
    int16x8_t _s0 = vaddq_s16((x0), (x7)), _d0 = vsubq_s16((x0), (x7)); \
    int16x8_t _s1 = vaddq_s16((x1), (x6)), _d1 = vsubq_s16((x1), (x6)); \
    int16x8_t _s2 = vaddq_s16((x2), (x5)), _d2 = vsubq_s16((x2), (x5)); \
    int16x8_t _s3 = vaddq_s16((x3), (x4)), _d3 = vsubq_s16((x3), (x4)); \
    int16x8_t _t0 = vaddq_s16(_s0, _s3), _t3 = vsubq_s16(_s0, _s3); \
    int16x8_t _t1 = vaddq_s16(_s1, _s2), _t2 = vsubq_s16(_s1, _s2); \
    (x0) = vaddq_s16(_t0, _t1); \
    (x4) = vsubq_s16(_t0, _t1); \
    int16x8_t _z1 = vqrdmulhq_n_s16(vaddq_s16(_t2, _t3), 23170); \
    (x2) = vaddq_s16(_t3, _z1); \
    (x6) = vsubq_s16(_t3, _z1); \
    int16x8_t _t4 = vaddq_s16(_d2, _d3); \
    int16x8_t _t5 = vaddq_s16(_d1, _d2); \
    int16x8_t _t6 = vaddq_s16(_d0, _d1); \
    int16x8_t _z5 = vqrdmulhq_n_s16(vsubq_s16(_t4, _t6), 12540); \
    int16x8_t _z2 = vaddq_s16(vqrdmulhq_n_s16(_t4, 17734), _z5); \
    int16x8_t _z4 = vaddq_s16(vaddq_s16(_t6, vqrdmulhq_n_s16(_t6, 10045)), _z5); \
    int16x8_t _z3 = vqrdmulhq_n_s16(_t5, 23170); \
    int16x8_t _z11 = vaddq_s16(_d0, _z3), _z13 = vsubq_s16(_d0, _z3); \
    (x5) = vaddq_s16(_z13, _z2); \
    (x3) = vsubq_s16(_z13, _z2); \
    (x1) = vaddq_s16(_z11, _z4); \
    (x7) = vsubq_s16(_z11, _z4); \
} while (0)

static inline int16x8_t neon_quant_row_s16(int16x8_t v, const uint16_t* inv_q_row, int clamp_1023) {
    uint16x8_t neg_mask = vcltq_s16(v, vdupq_n_s16(0));
    uint16x8_t av = vreinterpretq_u16_s16(vabsq_s16(v));
    uint16x8_t iq = vld1q_u16(inv_q_row);
    uint16x4_t qlo = vrshrn_n_u32(vmull_u16(vget_low_u16(av),  vget_low_u16(iq)),  16);
    uint16x4_t qhi = vrshrn_n_u32(vmull_u16(vget_high_u16(av), vget_high_u16(iq)), 16);
    uint16x8_t qv  = vcombine_u16(qlo, qhi);
    if (clamp_1023) {
        qv = vminq_u16(qv, vdupq_n_u16(1023));
    }
    int16x8_t qs = vreinterpretq_s16_u16(qv);
    return vbslq_s16(neg_mask, vnegq_s16(qs), qs);
}

void neon_fdct_quant_8x8(int16_t* blk, const uint16_t* inv_q_nat, JpegQuantBlock* qb) {
    int16x8_t r0 = vld1q_s16(blk + 0);
    int16x8_t r1 = vld1q_s16(blk + 8);
    int16x8_t r2 = vld1q_s16(blk + 16);
    int16x8_t r3 = vld1q_s16(blk + 24);
    int16x8_t r4 = vld1q_s16(blk + 32);
    int16x8_t r5 = vld1q_s16(blk + 40);
    int16x8_t r6 = vld1q_s16(blk + 48);
    int16x8_t r7 = vld1q_s16(blk + 56);

    // Row FDCT (transpose -> 8-wide 1D FDCT -> transpose back)
    NEON_TRANSPOSE_8x8_S16(r0, r1, r2, r3, r4, r5, r6, r7);
    NEON_FDCT_1D(r0, r1, r2, r3, r4, r5, r6, r7);
    NEON_TRANSPOSE_8x8_S16(r0, r1, r2, r3, r4, r5, r6, r7);

    // Column FDCT -> >> 3 (exact match to native_jpeg_writer.c fdct_1d!)
    NEON_FDCT_1D(r0, r1, r2, r3, r4, r5, r6, r7);
    r0 = vrshrq_n_s16(r0, 3); r1 = vrshrq_n_s16(r1, 3);
    r2 = vrshrq_n_s16(r2, 3); r3 = vrshrq_n_s16(r3, 3);
    r4 = vrshrq_n_s16(r4, 3); r5 = vrshrq_n_s16(r5, 3);
    r6 = vrshrq_n_s16(r6, 3); r7 = vrshrq_n_s16(r7, 3);

    // Preserve unquantized DC in blk[0] for zero-I/O preview extraction
    blk[0] = vgetq_lane_s16(r0, 0);

    // Vectorized reciprocal quantization across all 64 coefficients (8 at a time!)
    int16_t qnat[64];
    int16x8_t q0 = neon_quant_row_s16(r0, inv_q_nat + 0,  0);
    int16x8_t q1 = neon_quant_row_s16(r1, inv_q_nat + 8,  1);
    int16x8_t q2 = neon_quant_row_s16(r2, inv_q_nat + 16, 1);
    int16x8_t q3 = neon_quant_row_s16(r3, inv_q_nat + 24, 1);
    int16x8_t q4 = neon_quant_row_s16(r4, inv_q_nat + 32, 1);
    int16x8_t q5 = neon_quant_row_s16(r5, inv_q_nat + 40, 1);
    int16x8_t q6 = neon_quant_row_s16(r6, inv_q_nat + 48, 1);
    int16x8_t q7 = neon_quant_row_s16(r7, inv_q_nat + 56, 1);

    vst1q_s16(qnat + 0,  q0); vst1q_s16(qnat + 8,  q1);
    vst1q_s16(qnat + 16, q2); vst1q_s16(qnat + 24, q3);
    vst1q_s16(qnat + 32, q4); vst1q_s16(qnat + 40, q5);
    vst1q_s16(qnat + 48, q6); vst1q_s16(qnat + 56, q7);

    // Fast all-AC-zero check using NEON OR-reduction (huge speedup on chroma & smooth blocks!)
    int16x8_t ac_or = vorrq_s16(vorrq_s16(vorrq_s16(q1, q2), vorrq_s16(q3, q4)),
                                vorrq_s16(vorrq_s16(q5, q6), q7));
    int16x8_t q0_ac = vsetq_lane_s16(0, q0, 0);
    ac_or = vorrq_s16(ac_or, q0_ac);
    uint32x2_t p32 = vreinterpret_u32_s16(vorr_s16(vget_low_s16(ac_or), vget_high_s16(ac_or)));
    uint32_t nz_any = vget_lane_u32(p32, 0) | vget_lane_u32(p32, 1);

    qb->q[0] = qnat[0];
    if (nz_any == 0) {
        qb->last_nz = 0;
        return;
    }

    // Clamp AC row 0 entries to [-1023..1023]
    for (int k = 1; k < 8; k++) {
        if (qnat[k] > 1023) qnat[k] = 1023;
        else if (qnat[k] < -1023) qnat[k] = -1023;
    }

    int last_nz = 0;
    for (int i = 1; i < 64; i++) {
        int16_t v = qnat[NEON_ZIGZAG[i]];
        qb->q[i] = v;
        if (v != 0) last_nz = i;
    }
    qb->last_nz = (uint8_t)last_nz;
}

void neon_apply_luma_grain_mcu(int16_t y_blk[4][64], int global_x0, int global_y0,
                               const uint8_t grain_mid_lut[256], const int8_t grain_64x64[4096]) {
    int x_tile = (global_x0 >> 4);
    int16x8_t v_min = vdupq_n_s16(-128);
    int16x8_t v_max = vdupq_n_s16(127);

    for (int y = 0; y < 16; y++) {
        int gy = global_y0 + y;
        int b_idx = (y >= 8) ? 2 : 0;
        int sy8 = (y & 7) * 8;
        int phase = (int)(((uint32_t)gy * 2654435761U) >> 26);
        const int8_t* g1 = grain_64x64 + ((gy & 63) << 6);
        const int8_t* g2 = grain_64x64 + (((gy * 7 + x_tile) & 63) << 6);

        for (int half = 0; half < 2; half++) {
            int16_t* row = y_blk[b_idx + half] + sy8;
            int gx_base = global_x0 + (half << 3);
            int gx1 = gx_base & 63;
            int gx2 = (gx_base + phase) & 63;

            int8x8_t vn1 = vld1_s8(g1 + gx1); // gx_base is always 8-aligned so gx1 <= 56
            int8x8_t vn2;
            if (gx2 <= 56) {
                vn2 = vld1_s8(g2 + gx2);
            } else {
                int8_t n2_buf[8];
                for (int x = 0; x < 8; x++) n2_buf[x] = g2[(gx2 + x) & 63];
                vn2 = vld1_s8(n2_buf);
            }
            int16_t gain_buf[8];
            for (int x = 0; x < 8; x++) {
                gain_buf[x] = (int16_t)grain_mid_lut[(int)row[x] + 128];
            }
            int16x8_t v_y   = vld1q_s16(row);
            int16x8_t v_raw = vaddl_s8(vn1, vn2);
            int16x8_t v_g   = vld1q_s16(gain_buf);
            int16x8_t v_del = vshrq_n_s16(vmulq_s16(v_raw, v_g), 9);
            int16x8_t v_out = vminq_s16(vmaxq_s16(vaddq_s16(v_y, v_del), v_min), v_max);
            vst1q_s16(row, v_out);
        }
    }
}

#endif // __ARM_NEON__
