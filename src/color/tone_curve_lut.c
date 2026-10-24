#include "tone_curve_lut.h"
#include <math.h>

FujiResult lut_init_identity(ToneCurveLUT* lut) {
    if (!lut) return FUJI_ERR_NULL_POINTER;
    for (int i = 0; i < 256; i++) {
        lut->r[i] = (uint8_t)i;
        lut->g[i] = (uint8_t)i;
        lut->b[i] = (uint8_t)i;
    }
    return FUJI_SUCCESS;
}

FujiResult lut_generate_scurve(ToneCurveLUT* lut, int shadow_lift, int highlight_rolloff, int contrast_boost) {
    if (!lut) return FUJI_ERR_NULL_POINTER;

    for (int i = 0; i < 256; i++) {
        double x = (double)i / 255.0;
        
        // Basic parametric contrast sigmoid: (x - 0.5) * factor
        double contrast_factor = 1.0 + ((double)contrast_boost / 100.0);
        double centered = (x - 0.5) * contrast_factor;
        double s = 1.0 / (1.0 + exp(-6.0 * centered));
        
        // Shadow lift adjustment
        if (x < 0.3) {
            double shadow_weight = (0.3 - x) / 0.3;
            s += ((double)shadow_lift / 255.0) * shadow_weight;
        }

        // Highlight rolloff adjustment
        if (x > 0.7) {
            double highlight_weight = (x - 0.7) / 0.3;
            s -= ((double)highlight_rolloff / 255.0) * highlight_weight;
        }

        // Clamp
        if (s < 0.0) s = 0.0;
        if (s > 1.0) s = 1.0;

        uint8_t val = (uint8_t)(s * 255.0 + 0.5);
        lut->r[i] = val;
        lut->g[i] = val;
        lut->b[i] = val;
    }

    return FUJI_SUCCESS;
}
