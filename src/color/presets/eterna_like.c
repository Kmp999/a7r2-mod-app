#include "eterna_like.h"

void get_preset_eterna_matrix(Matrix3x3* mat) {
    if (!mat) return;
    mat->m[0][0] = 210; mat->m[0][1] = 25;  mat->m[0][2] = 21;
    mat->m[1][0] = 20;  mat->m[1][1] = 215; mat->m[1][2] = 21;
    mat->m[2][0] = 20;  mat->m[2][1] = 24;  mat->m[2][2] = 212;
}

void get_preset_eterna_lut(ToneCurveLUT* lut) {
    if (!lut) return;
    // Flat contrast (-10), soft rolloff (30), lifted shadows (20)
    lut_generate_scurve(lut, 20, 30, -10);
}
