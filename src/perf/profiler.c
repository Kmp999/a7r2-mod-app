#include "profiler.h"
#include <stdio.h>
#include <string.h>

static const char* STAGE_NAMES[STAGE_COUNT] = {
    "trigger_pressed",
    "image_available",
    "app_read_image",
    "app_decode_image",
    "app_downsample",
    "app_color_matrix",
    "app_tone_curve_lut",
    "app_saturation",
    "app_encode_output",
    "app_save_output",
    "app_ui_update"
};

void profiler_reset(ProfilerContext* prof) {
    if (!prof) return;
    memset(prof, 0, sizeof(ProfilerContext));
}

void profiler_stage_start(ProfilerContext* prof, ProfilerStage stage) {
    if (!prof || stage >= STAGE_COUNT) return;
    prof->stage_start_us[stage] = fuji_get_timestamp_us();
}

void profiler_stage_end(ProfilerContext* prof, ProfilerStage stage) {
    if (!prof || stage >= STAGE_COUNT) return;
    uint64_t now = fuji_get_timestamp_us();
    if (now >= prof->stage_start_us[stage]) {
        prof->stage_duration_us[stage] = now - prof->stage_start_us[stage];
    }
}

void profiler_finalize(ProfilerContext* prof) {
    if (!prof) return;
    // Pipeline duration is calculated from image_available onwards
    uint64_t total = 0;
    for (int i = STAGE_APP_READ_IMAGE; i <= STAGE_APP_UI_UPDATE; i++) {
        total += prof->stage_duration_us[i];
    }
    prof->total_pipeline_duration_us = total;
    prof->meets_500ms_budget = (total <= 500000ULL); // 500 ms = 500,000 us
}

void profiler_print_summary(const ProfilerContext* prof) {
    if (!prof) return;
    printf("\n================ TIMING PROFILER REPORT ================\n");
    for (int i = 0; i < STAGE_COUNT; i++) {
        double ms = (double)prof->stage_duration_us[i] / 1000.0;
        printf("  %-25s : %8.2f ms\n", STAGE_NAMES[i], ms);
    }
    printf("--------------------------------------------------------\n");
    double total_ms = (double)prof->total_pipeline_duration_us / 1000.0;
    printf("  TOTAL APP PIPELINE TIME   : %8.2f ms\n", total_ms);
    printf("  BUDGET TARGET (500 ms)    : %s\n", prof->meets_500ms_budget ? "PASSED [OK]" : "FAILED [EXCEEDED]");
    printf("========================================================\n\n");
}
