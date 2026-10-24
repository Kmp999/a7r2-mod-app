#ifndef FUJI_BENCHMARK_RUNNER_H
#define FUJI_BENCHMARK_RUNNER_H

#include "../color/color_engine.h"
#include "profiler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    int iterations;
    PresetId preset;
    int mode;
} BenchmarkConfig;

typedef struct {
    double avg_color_matrix_ms;
    double avg_tone_curve_lut_ms;
    double avg_downsample_ms;
    double avg_total_app_ms;
    int passed_500ms_target;
} BenchmarkResult;

FujiResult benchmark_run_suite(const BenchmarkConfig* config, BenchmarkResult* out_result);

#ifdef __cplusplus
}
#endif

#endif // FUJI_BENCHMARK_RUNNER_H
