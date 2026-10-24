#ifndef FUJI_TONE_CURVE_LUT_H
#define FUJI_TONE_CURVE_LUT_H

#include <stdint.h>
#include "../util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r[256];
    uint8_t g[256];
    uint8_t b[256];
} ToneCurveLUT;

// Initializes an identity (linear) LUT
FujiResult lut_init_identity(ToneCurveLUT* lut);

// Generates parameterized S-curve LUT
// shadow_lift: -50 to +50 (lift or crush shadows)
// highlight_rolloff: -50 to +50 (soften or compress highlights)
// contrast_boost: 0 to 100
FujiResult lut_generate_scurve(ToneCurveLUT* lut, int shadow_lift, int highlight_rolloff, int contrast_boost);

// In-place tone curve application on RGB pixel
static inline void lut_apply_rgb(const ToneCurveLUT* lut, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = lut->r[*r];
    *g = lut->g[*g];
    *b = lut->b[*b];
}

#ifdef __cplusplus
}
#endif

#endif // FUJI_TONE_CURVE_LUT_H
