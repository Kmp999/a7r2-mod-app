#include "astia_like.h"

void get_preset_astia_matrix(Matrix3x3* mat) {
    if (!mat) return;
    mat->m[0][0] = 248; mat->m[0][1] = 6;   mat->m[0][2] = 2;
    mat->m[1][0] = 2;   mat->m[1][1] = 254; mat->m[1][2] = 0;
    mat->m[2][0] = 0;   mat->m[2][1] = 4;   mat->m[2][2] = 252;
}

void get_preset_astia_lut(ToneCurveLUT* lut) {
    if (!lut) return;
    // Gentle contrast, soft highlights
    lut_generate_scurve(lut, 5, -10, 8);
}
