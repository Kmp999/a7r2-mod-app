#include "classic_neg_like.h"

void get_preset_classic_neg_matrix(Matrix3x3* mat) {
    if (!mat) return;
    mat->m[0][0] = 230; mat->m[0][1] = 16;  mat->m[0][2] = 10;
    mat->m[1][0] = -12; mat->m[1][1] = 270; mat->m[1][2] = -2;
    mat->m[2][0] = 10;  mat->m[2][1] = -15; mat->m[2][2] = 261;
}

void get_preset_classic_neg_lut(ToneCurveLUT* lut) {
    if (!lut) return;
    // Lifted shadows (15), compressed highlights (20), medium contrast
    lut_generate_scurve(lut, 15, 20, 25);
}
