#ifndef FUJI_PRESET_MANAGER_H
#define FUJI_PRESET_MANAGER_H

#include "fixed_point_matrix.h"
#include "tone_curve_lut.h"
#include "color_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    PresetId id;
    const char* name;
    Matrix3x3 matrix;
    ToneCurveLUT lut;
    uint32_t expected_cost_ms_720p;
} PresetDefinition;

FujiResult preset_manager_init(void);
const PresetDefinition* preset_manager_get(PresetId id);
int preset_manager_count(void);

#ifdef __cplusplus
}
#endif

#endif // FUJI_PRESET_MANAGER_H
