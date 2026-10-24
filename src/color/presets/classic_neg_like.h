#ifndef PRESET_CLASSIC_NEG_LIKE_H
#define PRESET_CLASSIC_NEG_LIKE_H

#include "../fixed_point_matrix.h"
#include "../tone_curve_lut.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Note: These are original approximations inspired by film photography aesthetics.
 * They are not official Fujifilm simulations and must not claim to reproduce proprietary Fujifilm data.
 *
 * Classic Neg-like:
 * - Faded highlights, lifted shadows
 * - Green/cyan shift, warm skin bias
 * - Expected cost: ~50ms at 720p
 */

void get_preset_classic_neg_matrix(Matrix3x3* mat);
void get_preset_classic_neg_lut(ToneCurveLUT* lut);

#ifdef __cplusplus
}
#endif

#endif // PRESET_CLASSIC_NEG_LIKE_H
