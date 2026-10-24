#include "provia_like.h"

void get_preset_provia_matrix(Matrix3x3* mat) {
    if (!mat) return;
    // Balanced Q8.8 Matrix (256 = 1.0)
    mat->m[0][0] = 266; mat->m[0][1] = -5;  mat->m[0][2] = -5;
    mat->m[1][0] = -5;  mat->m[1][1] = 266; mat->m[1][2] = -5;
    mat->m[2][0] = -5;  mat->m[2][1] = -5;  mat->m[2][2] = 266;
}

void get_preset_provia_lut(ToneCurveLUT* lut) {
    if (!lut) return;
    lut_generate_scurve(lut, 0, 0, 15);
}
