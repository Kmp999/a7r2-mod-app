#include "velvia_like.h"

void get_preset_velvia_matrix(Matrix3x3* mat) {
    if (!mat) return;
    mat->m[0][0] = 290; mat->m[0][1] = -20; mat->m[0][2] = -14;
    mat->m[1][0] = -16; mat->m[1][1] = 295; mat->m[1][2] = -23;
    mat->m[2][0] = -10; mat->m[2][1] = -25; mat->m[2][2] = 291;
}

void get_preset_velvia_lut(ToneCurveLUT* lut) {
    if (!lut) return;
    // Deep blacks, punchy highlights
    lut_generate_scurve(lut, -15, 10, 40);
}
