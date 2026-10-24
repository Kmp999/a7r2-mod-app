#ifndef FUJI_PROFILER_H
#define FUJI_PROFILER_H

#include <stdint.h>
#include "timestamp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STAGE_TRIGGER_PRESSED = 0,
    STAGE_IMAGE_AVAILABLE,
    STAGE_APP_READ_IMAGE,
    STAGE_APP_DECODE_IMAGE,
    STAGE_APP_DOWNSAMPLE,
    STAGE_APP_COLOR_MATRIX,
    STAGE_APP_TONE_CURVE_LUT,
    STAGE_APP_SATURATION,
    STAGE_APP_ENCODE_OUTPUT,
    STAGE_APP_SAVE_OUTPUT,
    STAGE_APP_UI_UPDATE,
    STAGE_COUNT
} ProfilerStage;

typedef struct {
    uint64_t stage_start_us[STAGE_COUNT];
    uint64_t stage_duration_us[STAGE_COUNT];
    uint64_t total_pipeline_duration_us;
    int meets_500ms_budget;
} ProfilerContext;

void profiler_reset(ProfilerContext* prof);
void profiler_stage_start(ProfilerContext* prof, ProfilerStage stage);
void profiler_stage_end(ProfilerContext* prof, ProfilerStage stage);
void profiler_finalize(ProfilerContext* prof);
void profiler_print_summary(const ProfilerContext* prof);

#ifdef __cplusplus
}
#endif

#endif // FUJI_PROFILER_H
