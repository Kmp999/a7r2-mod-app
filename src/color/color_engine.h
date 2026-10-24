#ifndef COLOR_ENGINE_H
#define COLOR_ENGINE_H

#include <stdint.h>
#include "fixed_point_matrix.h"
#include "tone_curve_lut.h"
#include "../util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mode flags
#define COLOR_MODE_MATRIX_ONLY 1
#define COLOR_MODE_MATRIX_LUT 2
#define COLOR_MODE_MATRIX_LUT_SATURATION 3
#define COLOR_MODE_MATRIX_LUT_SPLIT_TONE 4
#define COLOR_MODE_SAFE_FASTEST 5

// Preset IDs
typedef enum {
    PRESET_PROVIA_LIKE = 0,
    PRESET_ASTIA_LIKE,
    PRESET_VELVIA_LIKE,
    PRESET_CLASSIC_NEG_LIKE,
    PRESET_ETERNA_LIKE
} PresetId;

// Complete engine context
typedef struct {
    Matrix3x3 matrix;
    ToneCurveLUT lut;
    int mode;
    PresetId current_preset;
} ColorEngineContext;

// Initialize the color engine with a specific preset and mode
FujiResult color_engine_init(ColorEngineContext* ctx, PresetId preset, int mode);

// Fast in-place NV21 YUV processing (Direct for Live View / PreviewCallback)
FujiResult color_engine_process_nv21(ColorEngineContext* ctx, uint8_t* nv21_buffer, int width, int height);

// Fast in-place RGB24 processing (For decoded JPEG proxy or host testing)
FujiResult color_engine_process_rgb24(ColorEngineContext* ctx, uint8_t* rgb_buffer, int width, int height);

#ifdef __cplusplus
}
#endif

#endif // COLOR_ENGINE_H
