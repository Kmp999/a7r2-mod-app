#ifndef NEON_JPEG_KERNELS_H
#define NEON_JPEG_KERNELS_H

#include <stdint.h>
#include "native_jpeg_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runtime-dispatched ARMv7-A NEON SIMD JPEG kernels (compiled ONLY via .c.neon on armeabi-v7a).
 * Caller MUST check (has_neon != 0) from /proc/cpuinfo before invoking any function below.
 */

// 8-wide int16x8_t AAN 2D IDCT (8x8 DCT -> 8x8 uint8 pixels) using Q2 dequant table.
void neon_idct_8x8(const int16_t* in, uint8_t has_ac, const int16_t* dq_q2, const uint16_t* q_raw, uint8_t* out);

// 8-wide int16x8_t exact half-height vertical AAN 2D IDCT (8x8 DCT -> 8x4 vertically 2:1 averaged chroma).
void neon_idct_8x4_half_v(const int16_t* in, uint8_t has_ac, const int16_t* dq_q2, const uint16_t* q_raw, uint8_t* out32);

// 8-wide int16x8_t AAN 2D FDCT + NEON vectorized reciprocal quantization + zigzag scatter.
// Preserves unquantized DC coefficient in blk[0] (8 * block_avg) for zero-I/O preview extraction.
void neon_fdct_quant_8x8(int16_t* blk, const uint16_t* inv_q_nat, JpegQuantBlock* qb);

// Vectorized 8-pixel-wide luma grain pass directly on y_blk[4][64] (clarity == 0 fast path).
void neon_apply_luma_grain_mcu(int16_t y_blk[4][64], int global_x0, int global_y0,
                               const uint8_t grain_mid_lut[256], const int8_t grain_64x64[4096]);

#ifdef __cplusplus
}
#endif

#endif // NEON_JPEG_KERNELS_H
