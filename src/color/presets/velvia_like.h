#ifndef PRESET_VELVIA_LIKE_H
#define PRESET_VELVIA_LIKE_H

#include "../fixed_point_matrix.h"
#include "../tone_curve_lut.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Note: These are original approximations inspired by film photography aesthetics.
 * They are not official Fujifilm simulations and must not claim to reproduce proprietary Fujifilm data.
 *
 * Velvia-like:
 * - High contrast, rich saturation
 * - Strong blues and greens
 * - Expected cost: ~48ms at 720p
 */

void get_preset_velvia_matrix(Matrix3x3* mat);
void get_preset_velvia_lut(ToneCurveLUT* lut);

#ifdef __cplusplus
}
#endif

#endif // PRESET_VELVIA_LIKE_H
