#ifndef FUJI_FIXED_POINT_MATRIX_H
#define FUJI_FIXED_POINT_MATRIX_H

#include <stdint.h>

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
static inline void matrix_apply_rgb(const Matrix3x3* mat, uint8_t* r, uint8_t* g, uint8_t* b) {
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

#ifdef __cplusplus
}
#endif

#endif // FUJI_FIXED_POINT_MATRIX_H
