#ifndef PRESET_ETERNA_LIKE_H
#define PRESET_ETERNA_LIKE_H

#include "../fixed_point_matrix.h"
#include "../tone_curve_lut.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Note: These are original approximations inspired by film photography aesthetics.
 * They are not official Fujifilm simulations and must not claim to reproduce proprietary Fujifilm data.
 *
 * Eterna-like:
 * - Low contrast, soft highlights, muted saturation
 * - Cinematic tone
 * - Expected cost: ~42ms at 720p
 */

void get_preset_eterna_matrix(Matrix3x3* mat);
void get_preset_eterna_lut(ToneCurveLUT* lut);

#ifdef __cplusplus
}
#endif

#endif // PRESET_ETERNA_LIKE_H
