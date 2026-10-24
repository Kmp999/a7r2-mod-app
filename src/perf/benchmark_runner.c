#include "benchmark_runner.h"
#include "../util/memory_pool.h"
#include "../image/downscaler.h"
#include <stdlib.h>
#include <string.h>

FujiResult benchmark_run_suite(const BenchmarkConfig* config, BenchmarkResult* out_result) {
    if (!config || !out_result) return FUJI_ERR_NULL_POINTER;

    int w = config->width;
    int h = config->height;
    int iters = config->iterations > 0 ? config->iterations : 10;
    size_t frame_bytes = w * h * 3;

    // Use memory pool for synthetic frame
    static MemoryPool pool;
    memory_pool_init(&pool);
    uint8_t* buffer = memory_pool_acquire_buffer(&pool, frame_bytes);
    if (!buffer) return FUJI_ERR_OUT_OF_MEMORY;

    // Fill with synthetic test pattern
    for (size_t i = 0; i < frame_bytes; i++) {
        buffer[i] = (uint8_t)(i % 256);
    }

    ColorEngineContext ctx;
    color_engine_init(&ctx, config->preset, config->mode);

    ProfilerContext prof;
    uint64_t total_time_accum = 0;

    for (int i = 0; i < iters; i++) {
        profiler_reset(&prof);
        profiler_stage_start(&prof, STAGE_IMAGE_AVAILABLE);

        profiler_stage_start(&prof, STAGE_APP_READ_IMAGE);
        // Simulated zero-copy buffer check
        profiler_stage_end(&prof, STAGE_APP_READ_IMAGE);

        profiler_stage_start(&prof, STAGE_APP_COLOR_MATRIX);
        color_engine_process_rgb24(&ctx, buffer, w, h);
        profiler_stage_end(&prof, STAGE_APP_COLOR_MATRIX);

        profiler_stage_start(&prof, STAGE_APP_UI_UPDATE);
        // Simulated minimal state update
        profiler_stage_end(&prof, STAGE_APP_UI_UPDATE);

        profiler_finalize(&prof);
        total_time_accum += prof.total_pipeline_duration_us;
    }

    memory_pool_release_buffer(&pool, buffer);

    out_result->avg_total_app_ms = (double)(total_time_accum / iters) / 1000.0;
    out_result->avg_color_matrix_ms = out_result->avg_total_app_ms; // In combined pass
    out_result->avg_tone_curve_lut_ms = 0.0;
    out_result->avg_downsample_ms = 0.0;
    out_result->passed_500ms_target = (out_result->avg_total_app_ms <= 500.0);

    return FUJI_SUCCESS;
}
