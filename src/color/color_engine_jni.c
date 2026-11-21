#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "color_engine.h"
#include "lut_cube_17.h"
#include "native_jpeg_writer.h"
#include "native_jpeg_fast_stream.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ColorEngineJNI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ColorEngineJNI", __VA_ARGS__)

static ColorEngineContext g_ctx;
static Lut17Context g_lut17;
// Standard pthread_mutex_t guarantees 100% Android API 9/10/16 Bionic libc compatibility
static pthread_mutex_t g_lut_mutex = PTHREAD_MUTEX_INITIALIZER;

// Reusable native NV21 scratch buffer to eliminate GetPrimitiveArrayCritical Dalvik GC locks
static uint8_t* g_nv21_scratch = NULL;
static size_t g_nv21_scratch_cap = 0;
static pthread_mutex_t g_nv21_mutex = PTHREAD_MUTEX_INITIALIZER;

// Reusable native scratch buffer for int[] fallback strip grading (zero GC lock)
static int32_t* g_int_scratch = NULL;
static size_t g_int_scratch_cap = 0;
static pthread_mutex_t g_int_mutex = PTHREAD_MUTEX_INITIALIZER;

// Serializes dispatch to strip operations
static pthread_mutex_t g_strip_call_mutex = PTHREAD_MUTEX_INITIALIZER;

// Kernel-backed fork-join async JPEG strip encoder (overlaps Band N Huffman bitstream emission on Core 1
// with Band N+1 Skia decodeRegion on Core 0 using pthread_create + pthread_join, ZERO pthread_cond_wait!)
typedef struct {
    JpegStreamState* state;
    int is_argb;
    uint8_t* buf;
    size_t buf_cap;
    int width;
    int height;
    int row_offset;
    int use_quant_blocks;
    JpegQuantBlock* qblocks;
    int quant_mcus;
    int thread_active;
    int last_ok;
    pthread_t thread;
} AsyncEncodeTask;

static AsyncEncodeTask g_async_enc = {
    NULL, 0, NULL, 0, 0, 0, 0, 0, NULL, 0, 0, 1, (pthread_t)0
};

// Double-buffered quantized DCT block pools (eliminates 74MB memcpy and lets all 4 cores DCT-quantize in parallel)
static JpegQuantBlock* g_qstrip_work = NULL;
static JpegQuantBlock* g_qstrip_emit = NULL;
static size_t g_qstrip_cap = 0;

static void* encode_strip_once_fn(void* arg) {
    (void)arg;
    int ok;
    if (g_async_enc.use_quant_blocks) {
        ok = native_jpeg_emit_quantized_blocks(g_async_enc.state, g_async_enc.qblocks, g_async_enc.quant_mcus);
    } else {
        ok = g_async_enc.is_argb
            ? native_jpeg_stream_write_strip_argb8888(g_async_enc.state, (const uint32_t*)g_async_enc.buf,
                                                      g_async_enc.width, g_async_enc.height, g_async_enc.row_offset)
            : native_jpeg_stream_write_strip_rgb565(g_async_enc.state, (const uint16_t*)g_async_enc.buf,
                                                    g_async_enc.width, g_async_enc.height, g_async_enc.row_offset);
    }
    if (!ok) g_async_enc.last_ok = 0;
    return NULL;
}

static int encode_worker_wait(void) {
    if (g_async_enc.thread_active) {
        pthread_join(g_async_enc.thread, NULL);
        g_async_enc.thread_active = 0;
    }
    return g_async_enc.last_ok;
}

static void encode_worker_reset_for_open(void) {
    encode_worker_wait();
    g_async_enc.last_ok = 1;
}

static int encode_worker_submit_quantized(JpegStreamState* state, int total_mcus) {
    if (!state || !g_qstrip_work || total_mcus <= 0) return 0;
    if (!encode_worker_wait()) return 0;

    // Zero-copy pointer swap between 4-core quantized work buffer and background Huffman emit buffer
    JpegQuantBlock* tmp = g_qstrip_work;
    g_qstrip_work = g_qstrip_emit;
    g_qstrip_emit = tmp;

    g_async_enc.state = state;
    g_async_enc.use_quant_blocks = 1;
    g_async_enc.qblocks = g_qstrip_emit;
    g_async_enc.quant_mcus = total_mcus;
    if (pthread_create(&g_async_enc.thread, NULL, encode_strip_once_fn, NULL) == 0) {
        g_async_enc.thread_active = 1;
        return 1;
    }
    return native_jpeg_emit_quantized_blocks(state, g_qstrip_emit, total_mcus);
}

static int encode_worker_submit(JpegStreamState* state, int is_argb, const void* pixels,
                                int width, int height, int row_offset) {
    if (!state || !pixels || width <= 0 || height <= 0) return 0;
    if (!encode_worker_wait()) return 0;

    size_t bytes = (size_t)width * (size_t)height * (is_argb ? 4U : 2U);
    if (g_async_enc.buf_cap < bytes) {
        uint8_t* grown = (uint8_t*)realloc(g_async_enc.buf, bytes);
        if (grown != NULL) {
            g_async_enc.buf = grown;
            g_async_enc.buf_cap = bytes;
        }
    }
    if (g_async_enc.buf != NULL && g_async_enc.buf_cap >= bytes) {
        memcpy(g_async_enc.buf, pixels, bytes);
        g_async_enc.state = state;
        g_async_enc.is_argb = is_argb;
        g_async_enc.width = width;
        g_async_enc.height = height;
        g_async_enc.row_offset = row_offset;
        g_async_enc.use_quant_blocks = 0;
        if (pthread_create(&g_async_enc.thread, NULL, encode_strip_once_fn, NULL) == 0) {
            g_async_enc.thread_active = 1;
            return 1;
        }
    }
    return is_argb
        ? native_jpeg_stream_write_strip_argb8888(state, (const uint32_t*)pixels, width, height, row_offset)
        : native_jpeg_stream_write_strip_rgb565(state, (const uint16_t*)pixels, width, height, row_offset);
}

// 4-Core SMP Parallel Strip Grader + Fused 2D AAN FDCT & Quantizer for Sony A7R II Quad-Core ARM Cortex-A5
// Uses scoped fork-join (pthread_create + pthread_join) with zero condition variables.
typedef struct {
    int is_argb;
    void* pixels;
    int width;
    int height;
    int row_offset;
    int grain_strength;
    int clarity_strength;
    const JpegStreamState* jpeg_state;
    JpegQuantBlock* qblocks_base;
    int strip_h;
    int mcus_x;
    int mcu_row_start;
    int mcu_row_end;
} GradeSliceTask;

static void* grade_slice_thread_fn(void* arg) {
    GradeSliceTask* t = (GradeSliceTask*)arg;
    if (t->jpeg_state != NULL && t->qblocks_base != NULL) {
        // Fused L2-cached 16-row (1 MCU row) 3D LUT grading + RGB->YCbCr + 2D AAN FDCT + Quantization + ZigZag
        for (int m = t->mcu_row_start; m < t->mcu_row_end; m++) {
            int my = m << 4;
            int cur_h = t->strip_h - my;
            if (cur_h <= 0) break;
            if (cur_h > 16) cur_h = 16;
            JpegQuantBlock* out_qb = t->qblocks_base + (size_t)m * (size_t)t->mcus_x * 6U;
            if (t->is_argb) {
                uint32_t* row_ptr = ((uint32_t*)t->pixels) + (size_t)my * (size_t)t->width;
                lut17_process_argb8888_advanced_offset(&g_lut17, row_ptr, t->width, cur_h,
                                                       t->row_offset + my, t->grain_strength, t->clarity_strength);
                native_jpeg_quantize_mcu_row_argb8888(t->jpeg_state, (const uint32_t*)t->pixels,
                                                      t->width, t->strip_h, t->row_offset, my, out_qb);
            } else {
                uint16_t* row_ptr = ((uint16_t*)t->pixels) + (size_t)my * (size_t)t->width;
                lut17_process_rgb565_advanced_offset(&g_lut17, row_ptr, t->width, cur_h,
                                                     t->row_offset + my, t->grain_strength, t->clarity_strength);
                native_jpeg_quantize_mcu_row_rgb565(t->jpeg_state, (const uint16_t*)t->pixels,
                                                    t->width, t->strip_h, t->row_offset, my, out_qb);
            }
        }
        return NULL;
    }
    if (t->is_argb) {
        lut17_process_argb8888_advanced_offset(&g_lut17, (uint32_t*)t->pixels, t->width, t->height,
                                               t->row_offset, t->grain_strength, t->clarity_strength);
    } else {
        lut17_process_rgb565_advanced_offset(&g_lut17, (uint16_t*)t->pixels, t->width, t->height,
                                             t->row_offset, t->grain_strength, t->clarity_strength);
    }
    return NULL;
}

static void grade_strip_parallel(int is_argb, void* pixels, int width, int height,
                                 int row_offset, int grain_strength, int clarity_strength) {
    init_static_grain_table();
    pthread_mutex_lock(&g_strip_call_mutex);
    pthread_mutex_lock(&g_lut_mutex);

    if (height < 32) {
        if (is_argb) {
            lut17_process_argb8888_advanced_offset(&g_lut17, (uint32_t*)pixels, width, height, row_offset, grain_strength, clarity_strength);
        } else {
            lut17_process_rgb565_advanced_offset(&g_lut17, (uint16_t*)pixels, width, height, row_offset, grain_strength, clarity_strength);
        }
        pthread_mutex_unlock(&g_lut_mutex);
        pthread_mutex_unlock(&g_strip_call_mutex);
        return;
    }

    // Partition strip across all 4 ARM Cortex-A5 cores (even-row aligned for Bayer 2x2)
    GradeSliceTask tasks[4];
    pthread_t threads[3];
    int spawned[3] = { 0, 0, 0 };
    int base_h = (height >> 2) & ~1;
    if (base_h < 8) base_h = 8;

    int cur_y = 0;
    for (int k = 0; k < 4; k++) {
        int slice_h = (k == 3) ? (height - cur_y) : base_h;
        if (slice_h < 0) slice_h = 0;
        memset(&tasks[k], 0, sizeof(GradeSliceTask));
        tasks[k].is_argb = is_argb;
        tasks[k].pixels = is_argb
            ? (void*)(((uint32_t*)pixels) + (size_t)cur_y * (size_t)width)
            : (void*)(((uint16_t*)pixels) + (size_t)cur_y * (size_t)width);
        tasks[k].width = width;
        tasks[k].height = slice_h;
        tasks[k].row_offset = row_offset + cur_y;
        tasks[k].grain_strength = grain_strength;
        tasks[k].clarity_strength = clarity_strength;
        cur_y += slice_h;
    }

    for (int k = 1; k < 4; k++) {
        if (tasks[k].height > 0 && pthread_create(&threads[k - 1], NULL, grade_slice_thread_fn, &tasks[k]) == 0) {
            spawned[k - 1] = 1;
        }
    }
    if (tasks[0].height > 0) grade_slice_thread_fn(&tasks[0]);
    for (int k = 1; k < 4; k++) {
        if (tasks[k].height > 0 && !spawned[k - 1]) grade_slice_thread_fn(&tasks[k]);
    }
    for (int k = 1; k < 4; k++) {
        if (spawned[k - 1]) pthread_join(threads[k - 1], NULL);
    }

    pthread_mutex_unlock(&g_lut_mutex);
    pthread_mutex_unlock(&g_strip_call_mutex);
}

static int grade_and_encode_strip_parallel(JpegStreamState* state, int is_argb, void* pixels, int width, int height,
                                           int row_offset, int grain_strength, int clarity_strength) {
    if (!state || !pixels || width <= 0 || height <= 0) return 0;
    int stream_w = native_jpeg_get_stream_width(state);
    int stream_h = native_jpeg_get_stream_height(state);
    if (width != stream_w || row_offset >= stream_h) return 1;

    int eff_h = height;
    if (row_offset + eff_h > stream_h) eff_h = stream_h - row_offset;
    int num_mcu_rows = (eff_h + 15) >> 4;
    int mcus_x = (width + 15) >> 4;
    int total_mcus = num_mcu_rows * mcus_x;
    size_t needed_blocks = (size_t)total_mcus * 6U;

    init_static_grain_table();
    pthread_mutex_lock(&g_strip_call_mutex);

    if (g_qstrip_cap < needed_blocks) {
        if (!encode_worker_wait()) {
            pthread_mutex_unlock(&g_strip_call_mutex);
            return 0;
        }
        JpegQuantBlock* w_grown = (JpegQuantBlock*)realloc(g_qstrip_work, needed_blocks * sizeof(JpegQuantBlock));
        JpegQuantBlock* e_grown = (JpegQuantBlock*)realloc(g_qstrip_emit, needed_blocks * sizeof(JpegQuantBlock));
        if (w_grown != NULL) g_qstrip_work = w_grown;
        if (e_grown != NULL) g_qstrip_emit = e_grown;
        if (w_grown != NULL && e_grown != NULL) {
            g_qstrip_cap = needed_blocks;
        }
    }

    if (g_qstrip_work == NULL || g_qstrip_emit == NULL || g_qstrip_cap < needed_blocks) {
        pthread_mutex_unlock(&g_strip_call_mutex);
        grade_strip_parallel(is_argb, pixels, width, height, row_offset, grain_strength, clarity_strength);
        return encode_worker_submit(state, is_argb, pixels, width, height, row_offset);
    }

    pthread_mutex_lock(&g_lut_mutex);

    GradeSliceTask tasks[4];
    pthread_t threads[3];
    int spawned[3] = { 0, 0, 0 };
    int cur_m = 0;
    for (int k = 0; k < 4; k++) {
        int rem_m = num_mcu_rows - cur_m;
        int rem_cores = 4 - k;
        int slice_m = (rem_m > 0) ? ((rem_m + rem_cores - 1) / rem_cores) : 0;
        memset(&tasks[k], 0, sizeof(GradeSliceTask));
        tasks[k].is_argb = is_argb;
        tasks[k].pixels = pixels;
        tasks[k].width = width;
        tasks[k].strip_h = height;
        tasks[k].row_offset = row_offset;
        tasks[k].grain_strength = grain_strength;
        tasks[k].clarity_strength = clarity_strength;
        tasks[k].jpeg_state = state;
        tasks[k].qblocks_base = g_qstrip_work;
        tasks[k].mcus_x = mcus_x;
        tasks[k].mcu_row_start = cur_m;
        tasks[k].mcu_row_end = cur_m + slice_m;
        cur_m += slice_m;
    }

    for (int k = 1; k < 4; k++) {
        if (tasks[k].mcu_row_end > tasks[k].mcu_row_start) {
            if (pthread_create(&threads[k - 1], NULL, grade_slice_thread_fn, &tasks[k]) == 0) {
                spawned[k - 1] = 1;
            }
        }
    }
    if (tasks[0].mcu_row_end > tasks[0].mcu_row_start) {
        grade_slice_thread_fn(&tasks[0]);
    }
    for (int k = 1; k < 4; k++) {
        if (tasks[k].mcu_row_end > tasks[k].mcu_row_start && !spawned[k - 1]) {
            grade_slice_thread_fn(&tasks[k]);
        }
    }
    for (int k = 1; k < 4; k++) {
        if (spawned[k - 1]) {
            pthread_join(threads[k - 1], NULL);
        }
    }

    pthread_mutex_unlock(&g_lut_mutex);
    int ok = encode_worker_submit_quantized(state, total_mcus);
    pthread_mutex_unlock(&g_strip_call_mutex);
    return ok;
}

// 4x4 Bayer Ordered Dithering Matrix for RGB_565 banding reduction
static const int8_t BAYER_4X4[4][4] = {
    { -4,  2, -3,  3 },
    {  4, -2,  5, -1 },
    { -3,  3, -5,  2 },
    {  5, -1,  4, -2 }
};

// 64x64 Procedural Grain Lookup Table
static int8_t g_grainLut[64][64];
static int g_grainInitialized = 0;

static void init_grain_lut(void) {
    if (g_grainInitialized) return;
    int x, y;
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            // High-frequency pseudo-random integer noise between -3 and +3
            g_grainLut[y][x] = (int8_t)((((y * 13 + x * 37) ^ (x * y * 7)) % 7) - 3);
        }
    }
    g_grainInitialized = 1;
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_initEngine(JNIEnv *env, jclass clazz, jint presetId, jint mode) {
    color_engine_init(&g_ctx, (PresetId)presetId, mode);
    init_grain_lut();
    LOGI("Engine initialized with preset %d, mode %d", presetId, mode);
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_processNV21(JNIEnv *env, jclass clazz, jbyteArray nv21Array, jint width, jint height) {
    if (nv21Array == NULL || width <= 0 || height <= 0) {
        LOGE("Invalid arguments provided to processNV21");
        return;
    }

    jbyte* buffer_ptr = (*env)->GetPrimitiveArrayCritical(env, nv21Array, 0);
    if (buffer_ptr == NULL) {
        LOGE("Failed to get critical array pointer");
        return;
    }

    color_engine_process_nv21(&g_ctx, (uint8_t*)buffer_ptr, width, height);
    (*env)->ReleasePrimitiveArrayCritical(env, nv21Array, buffer_ptr, 0);
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_processRGB565(JNIEnv *env, jclass clazz, jshortArray rgb565Array, jint width, jint height) {
    if (rgb565Array == NULL || width <= 0 || height <= 0) {
        LOGE("Invalid arguments provided to processRGB565");
        return;
    }

    jshort* buffer_ptr = (*env)->GetPrimitiveArrayCritical(env, rgb565Array, 0);
    if (buffer_ptr == NULL) {
        LOGE("Failed to get critical array pointer for RGB565");
        return;
    }

    color_engine_process_rgb565(&g_ctx, (uint16_t*)buffer_ptr, width, height);
    (*env)->ReleasePrimitiveArrayCritical(env, rgb565Array, buffer_ptr, 0);
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_precomputeLut17(JNIEnv *env, jclass clazz,
                                                                        jshortArray ccmQ8Array,
                                                                        jbyteArray toneCurveR,
                                                                        jbyteArray toneCurveG,
                                                                        jbyteArray toneCurveB) {
    if (ccmQ8Array == NULL || toneCurveR == NULL || toneCurveG == NULL || toneCurveB == NULL) {
        LOGE("Invalid parameters to precomputeLut17");
        return;
    }

    if ((*env)->GetArrayLength(env, ccmQ8Array) < 9 ||
        (*env)->GetArrayLength(env, toneCurveR) < 256 ||
        (*env)->GetArrayLength(env, toneCurveG) < 256 ||
        (*env)->GetArrayLength(env, toneCurveB) < 256) {
        LOGE("Array length mismatch in precomputeLut17");
        return;
    }

    int16_t ccm[9];
    uint8_t r_curve[256];
    uint8_t g_curve[256];
    uint8_t b_curve[256];

    (*env)->GetShortArrayRegion(env, ccmQ8Array, 0, 9, (jshort*)ccm);
    (*env)->GetByteArrayRegion(env, toneCurveR, 0, 256, (jbyte*)r_curve);
    (*env)->GetByteArrayRegion(env, toneCurveG, 0, 256, (jbyte*)g_curve);
    (*env)->GetByteArrayRegion(env, toneCurveB, 0, 256, (jbyte*)b_curve);

    pthread_mutex_lock(&g_lut_mutex);
    lut17_precompute(&g_lut17, ccm, r_curve, g_curve, b_curve);
    pthread_mutex_unlock(&g_lut_mutex);
    LOGI("17x17x17 3D LUT precomputed successfully");
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_precomputeLut17WithBase(JNIEnv *env, jclass clazz,
                                                                                  jbyteArray baseLutArray,
                                                                                  jshortArray ccmQ8Array,
                                                                                  jbyteArray toneCurveR,
                                                                                  jbyteArray toneCurveG,
                                                                                  jbyteArray toneCurveB) {
    if (ccmQ8Array == NULL || toneCurveR == NULL || toneCurveG == NULL || toneCurveB == NULL) {
        LOGE("Invalid parameters to precomputeLut17WithBase");
        return;
    }

    if ((*env)->GetArrayLength(env, ccmQ8Array) < 9 ||
        (*env)->GetArrayLength(env, toneCurveR) < 256 ||
        (*env)->GetArrayLength(env, toneCurveG) < 256 ||
        (*env)->GetArrayLength(env, toneCurveB) < 256) {
        LOGE("Array length mismatch in precomputeLut17WithBase");
        return;
    }

    int16_t ccm[9];
    uint8_t r_curve[256];
    uint8_t g_curve[256];
    uint8_t b_curve[256];

    (*env)->GetShortArrayRegion(env, ccmQ8Array, 0, 9, (jshort*)ccm);
    (*env)->GetByteArrayRegion(env, toneCurveR, 0, 256, (jbyte*)r_curve);
    (*env)->GetByteArrayRegion(env, toneCurveG, 0, 256, (jbyte*)g_curve);
    (*env)->GetByteArrayRegion(env, toneCurveB, 0, 256, (jbyte*)b_curve);

    uint8_t* base_ptr = NULL;
    if (baseLutArray != NULL) {
        jsize baseLen = (*env)->GetArrayLength(env, baseLutArray);
        if (baseLen >= (jsize)(17 * 17 * 17 * 3)) {
            base_ptr = (uint8_t*)(*env)->GetByteArrayElements(env, baseLutArray, NULL);
        }
    }

    pthread_mutex_lock(&g_lut_mutex);
    lut17_precompute_with_base(&g_lut17, base_ptr, ccm, r_curve, g_curve, b_curve);
    pthread_mutex_unlock(&g_lut_mutex);

    if (base_ptr != NULL) {
        (*env)->ReleaseByteArrayElements(env, baseLutArray, (jbyte*)base_ptr, JNI_ABORT);
    }
    LOGI("17x17x17 3D LUT with base precomputed successfully");
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_loadLut17Table(JNIEnv *env, jclass clazz, jbyteArray tableArray) {
    if (tableArray == NULL) return;
    jsize len = (*env)->GetArrayLength(env, tableArray);
    if (len < (jsize)sizeof(g_lut17.table)) return;

    pthread_mutex_lock(&g_lut_mutex);
    (*env)->GetByteArrayRegion(env, tableArray, 0, sizeof(g_lut17.table), (jbyte*)g_lut17.table);
    pthread_mutex_unlock(&g_lut_mutex);
    LOGI("17x17x17 3D LUT direct table loaded (%zu bytes)", sizeof(g_lut17.table));
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_applyLut17RGB565(JNIEnv *env, jclass clazz,
                                                                         jshortArray rgb565Array,
                                                                         jint pixelCount) {
    if (rgb565Array == NULL || pixelCount <= 0) {
        return;
    }

    pthread_mutex_lock(&g_lut_mutex);
    jshort* buf_ptr = (*env)->GetPrimitiveArrayCritical(env, rgb565Array, 0);
    if (buf_ptr == NULL) {
        pthread_mutex_unlock(&g_lut_mutex);
        LOGE("Failed to get critical array pointer for applyLut17RGB565");
        return;
    }

    lut17_process_rgb565(&g_lut17, (uint16_t*)buf_ptr, pixelCount);
    (*env)->ReleasePrimitiveArrayCritical(env, rgb565Array, buf_ptr, 0);
    pthread_mutex_unlock(&g_lut_mutex);
}

/**
 * Direct ByteBuffer Zero-Copy Implementation (Requirement 4 & 5).
 * Completely eliminates Dalvik GC locks and applies 4x4 Bayer Dithering + Grain.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_applyLut17DirectRGB565(JNIEnv *env, jclass clazz,
                                                                                    jobject directBuffer,
                                                                                    jint width, jint height) {
    if (directBuffer == NULL || width <= 0 || height <= 0) {
        return JNI_FALSE;
    }

    init_grain_lut();

    uint16_t* buffer = (uint16_t*)(*env)->GetDirectBufferAddress(env, directBuffer);
    if (buffer == NULL) {
        LOGE("Failed to get DirectBuffer address for Zero-Copy grading");
        return JNI_FALSE;
    }

    pthread_mutex_lock(&g_lut_mutex);
    int x, y;
    for (y = 0; y < height; y++) {
        const int8_t* bayerRow = BAYER_4X4[y & 3];
        const int8_t* grainRow = g_grainLut[y & 63];
        int rowOffset = y * width;

        for (x = 0; x < width; x++) {
            int idx = rowOffset + x;
            uint16_t px = buffer[idx];

            // Extract 5-6-5 bits expanded to 8-bit without integer divisions
            uint8_t r, g, b;
            unpack_rgb565_fast(px, &r, &g, &b);

            // 17x17x17 3D LUT Interpolation
            uint8_t or_val, og_val, ob_val;
            lut17_interpolate_pixel(&g_lut17, r, g, b, &or_val, &og_val, &ob_val);

            // 4x4 Bayer Ordered Dither + 64x64 Grain
            int dither = bayerRow[x & 3] + grainRow[x & 63];
            int dr = or_val + dither;
            int dg = og_val + (dither >> 1);
            int db = ob_val + dither;

            if (dr < 0) dr = 0; else if (dr > 255) dr = 255;
            if (dg < 0) dg = 0; else if (dg > 255) dg = 255;
            if (db < 0) db = 0; else if (db > 255) db = 255;

            // Pack back down into RGB_565 (5-6-5 bits)
            buffer[idx] = (uint16_t)(((dr >> 3) << 11) | ((dg >> 2) << 5) | (db >> 3));
        }
    }

    pthread_mutex_unlock(&g_lut_mutex);
    return JNI_TRUE;
}

/**
 * Fused Real-time Live View Kernel (NV21 to LUT RGB_565).
 * Uses GetByteArrayRegion into a reusable native scratch buffer (ZERO GetPrimitiveArrayCritical
 * Dalvik GC lock!), processes 2 pixels per chroma fetch, and hoists mode branches out of inner loop.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeGradePreviewNV21(JNIEnv *env, jclass clazz,
                                                                                     jbyteArray nv21Array,
                                                                                     jobject rgb565Direct,
                                                                                     jint width, jint height,
                                                                                     jint splitScreenMode,
                                                                                     jint grainLevel) {
    if (nv21Array == NULL || rgb565Direct == NULL || width <= 0 || height <= 0) {
        return JNI_FALSE;
    }

    init_grain_lut();

    uint16_t* out_rgb = (uint16_t*)(*env)->GetDirectBufferAddress(env, rgb565Direct);
    if (out_rgb == NULL) {
        return JNI_FALSE;
    }

    size_t frameSize = (size_t)width * (size_t)height;
    size_t uvBytes = frameSize >> 1;
    size_t nv21Bytes = frameSize + uvBytes;
    if ((size_t)(*env)->GetArrayLength(env, nv21Array) < nv21Bytes) {
        return JNI_FALSE;
    }
    if ((*env)->GetDirectBufferCapacity(env, rgb565Direct) < (jlong)(frameSize * 2)) {
        return JNI_FALSE;
    }

    pthread_mutex_lock(&g_nv21_mutex);
    if (g_nv21_scratch_cap < nv21Bytes) {
        uint8_t* grown = (uint8_t*)realloc(g_nv21_scratch, nv21Bytes);
        if (grown == NULL) {
            pthread_mutex_unlock(&g_nv21_mutex);
            return JNI_FALSE;
        }
        g_nv21_scratch = grown;
        g_nv21_scratch_cap = nv21Bytes;
    }

    // Copy NV21 into native scratch without locking Dalvik GC (unlike GetPrimitiveArrayCritical)
    (*env)->GetByteArrayRegion(env, nv21Array, 0, (jsize)nv21Bytes, (jbyte*)g_nv21_scratch);

    const uint8_t* yPlane = g_nv21_scratch;
    const uint8_t* uvPlane = g_nv21_scratch + frameSize;
    int midX = width >> 1;

    pthread_mutex_lock(&g_lut_mutex);
    int x, y;
    for (y = 0; y < height; y++) {
        int uvOffset = (y >> 1) * width;
        const int8_t* bayerRow = BAYER_4X4[y & 3];
        const int8_t* grainRow = g_grainLut[y & 63];
        int yOffset = y * width;

        // Process 2 pixels horizontally per chroma pair (halves UV loads & chroma multiplies)
        for (x = 0; x < width; x += 2) {
            size_t uvIndex = (size_t)(uvOffset + x);
            int V = (uvIndex < uvBytes) ? ((int)uvPlane[uvIndex] - 128) : 0;
            int U = (uvIndex + 1 < uvBytes) ? ((int)uvPlane[uvIndex + 1] - 128) : 0;
            int r_uv = 409 * V + 128;
            int g_uv = -100 * U - 208 * V + 128;
            int b_uv = 516 * U + 128;

            int lim = (x + 1 < width) ? 2 : 1;
            for (int k = 0; k < lim; k++) {
                int cx = x + k;
                int yIndex = yOffset + cx;

                if (splitScreenMode == 1 && (cx == midX || cx == midX - 1)) {
                    out_rgb[yIndex] = 0xFDE0;
                    continue;
                }

                int Y = (int)yPlane[yIndex] - 16;
                if (Y < 0) Y = 0;
                int y298 = 298 * Y;

                int r0 = (y298 + r_uv) >> 8;
                int g0 = (y298 + g_uv) >> 8;
                int b0 = (y298 + b_uv) >> 8;

                uint8_t r = (uint8_t)(r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
                uint8_t g = (uint8_t)(g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
                uint8_t b = (uint8_t)(b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));

                int dither = bayerRow[cx & 3];
                int dr, dg, db;

                if (splitScreenMode == 0 || (splitScreenMode == 1 && cx > midX)) {
                    uint8_t or_val, og_val, ob_val;
                    lut17_interpolate_pixel(&g_lut17, r, g, b, &or_val, &og_val, &ob_val);
                    if (grainLevel > 0) {
                        dither += (grainRow[cx & 63] * grainLevel);
                    }
                    dr = or_val + dither;
                    dg = og_val + (dither >> 1);
                    db = ob_val + dither;
                } else {
                    dr = r + dither;
                    dg = g + (dither >> 1);
                    db = b + dither;
                }

                if (dr < 0) dr = 0; else if (dr > 255) dr = 255;
                if (dg < 0) dg = 0; else if (dg > 255) dg = 255;
                if (db < 0) db = 0; else if (db > 255) db = 255;

                out_rgb[yIndex] = (uint16_t)(((dr >> 3) << 11) | ((dg >> 2) << 5) | (db >> 3));
            }
        }
    }

    pthread_mutex_unlock(&g_lut_mutex);
    pthread_mutex_unlock(&g_nv21_mutex);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeGradeCaptureRGB565(JNIEnv *env, jclass clazz,
                                                                                       jobject directBuffer,
                                                                                       jint width, jint height) {
    return Java_com_fujilite_a7r2_ColorEngine_applyLut17DirectRGB565(env, clazz, directBuffer, width, height);
}

/**
 * Pure JNI Direct ByteBuffer grading with Grain and Clarity.
 * Zero external library dependencies (does not require libjnigraphics).
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeApplyLutAndGrainDirect(
    JNIEnv *env, jclass clazz, jobject directBuffer, jint width, jint height, jint grainStrength, jint clarityStrength) {
    if (directBuffer == NULL || width <= 0 || height <= 0) {
        LOGE("nativeApplyLutAndGrainDirect: Invalid arguments");
        return JNI_FALSE;
    }

    jlong cap = (*env)->GetDirectBufferCapacity(env, directBuffer);
    if (cap >= (jlong)width * height * 4) {
        uint32_t* pixels = (uint32_t*)(*env)->GetDirectBufferAddress(env, directBuffer);
        if (pixels == NULL) return JNI_FALSE;
        pthread_mutex_lock(&g_lut_mutex);
        lut17_process_argb8888_advanced_offset(&g_lut17, pixels, (int)width, (int)height, 0,
                                               (int)grainStrength, (int)clarityStrength);
        pthread_mutex_unlock(&g_lut_mutex);
        return JNI_TRUE;
    } else {
        uint16_t* pixels = (uint16_t*)(*env)->GetDirectBufferAddress(env, directBuffer);
        if (pixels == NULL) {
            LOGE("nativeApplyLutAndGrainDirect: GetDirectBufferAddress returned null");
            return JNI_FALSE;
        }

        pthread_mutex_lock(&g_lut_mutex);
        lut17_process_rgb565_advanced(&g_lut17, pixels, (int)width, (int)height,
                                      (int)grainStrength, (int)clarityStrength);
        pthread_mutex_unlock(&g_lut_mutex);
        return JNI_TRUE;
    }
}

/**
 * Pure JNI Direct ByteBuffer grading with Grain, Clarity, and scanline row offset.
 * Guarantees zero visible seams across strip boundaries. Supports ARGB_8888 and RGB_565.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeApplyLutAndGrainDirectOffset(
    JNIEnv *env, jclass clazz, jobject directBuffer, jint width, jint height, jint rowOffset, jint grainStrength, jint clarityStrength) {
    if (directBuffer == NULL || width <= 0 || height <= 0) {
        LOGE("nativeApplyLutAndGrainDirectOffset: Invalid arguments");
        return JNI_FALSE;
    }

    jlong cap = (*env)->GetDirectBufferCapacity(env, directBuffer);
    if (cap >= (jlong)width * height * 4) {
        uint32_t* pixels = (uint32_t*)(*env)->GetDirectBufferAddress(env, directBuffer);
        if (pixels == NULL) return JNI_FALSE;
        pthread_mutex_lock(&g_lut_mutex);
        lut17_process_argb8888_advanced_offset(&g_lut17, pixels, (int)width, (int)height, (int)rowOffset,
                                               (int)grainStrength, (int)clarityStrength);
        pthread_mutex_unlock(&g_lut_mutex);
        return JNI_TRUE;
    } else {
        uint16_t* pixels = (uint16_t*)(*env)->GetDirectBufferAddress(env, directBuffer);
        if (pixels == NULL) {
            LOGE("nativeApplyLutAndGrainDirectOffset: GetDirectBufferAddress returned null");
            return JNI_FALSE;
        }

        pthread_mutex_lock(&g_lut_mutex);
        lut17_process_rgb565_advanced_offset(&g_lut17, pixels, (int)width, (int)height, (int)rowOffset,
                                             (int)grainStrength, (int)clarityStrength);
        pthread_mutex_unlock(&g_lut_mutex);
        return JNI_TRUE;
    }
}

/**
 * Opens a streaming JPEG writer session for writing full-resolution (42.4MP: 7952x5304) JPEGs in strips.
 */
JNIEXPORT jlong JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeOpenJpegStream(
    JNIEnv *env, jclass clazz, jstring outputPath, jint width, jint height, jint quality) {
    if (outputPath == NULL || width <= 0 || height <= 0) {
        LOGE("nativeOpenJpegStream: Invalid parameters");
        return 0;
    }

    const char* path = (*env)->GetStringUTFChars(env, outputPath, NULL);
    if (path == NULL) {
        LOGE("nativeOpenJpegStream: Failed to extract outputPath UTF chars");
        return 0;
    }

    JpegStreamState* state = native_jpeg_stream_open(path, (int)width, (int)height, (int)quality);
    (*env)->ReleaseStringUTFChars(env, outputPath, path);

    if (state == NULL) {
        LOGE("nativeOpenJpegStream: native_jpeg_stream_open failed");
        return 0;
    }

    encode_worker_reset_for_open();
    return (jlong)(intptr_t)state;
}

/**
 * Opens a streaming JPEG writer session embedding raw EXIF APP1 bytes inline in the header
 * (eliminates post-write full-file ExifInterface rewrite on FAT32 SD cards).
 */
JNIEXPORT jlong JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeOpenJpegStreamWithExif(
    JNIEnv *env, jclass clazz, jstring outputPath, jint width, jint height, jint quality, jbyteArray exifApp1) {
    if (outputPath == NULL || width <= 0 || height <= 0) {
        LOGE("nativeOpenJpegStreamWithExif: Invalid parameters");
        return 0;
    }

    const char* path = (*env)->GetStringUTFChars(env, outputPath, NULL);
    if (path == NULL) {
        return 0;
    }

    uint8_t* exif_ptr = NULL;
    size_t exif_len = 0;
    if (exifApp1 != NULL) {
        jsize len = (*env)->GetArrayLength(env, exifApp1);
        if (len >= 4) {
            exif_ptr = (uint8_t*)(*env)->GetByteArrayElements(env, exifApp1, NULL);
            if (exif_ptr != NULL) {
                exif_len = (size_t)len;
            }
        }
    }

    JpegStreamState* state = native_jpeg_stream_open_with_exif(
        path, (int)width, (int)height, (int)quality, exif_ptr, exif_len);

    if (exif_ptr != NULL) {
        (*env)->ReleaseByteArrayElements(env, exifApp1, (jbyte*)exif_ptr, JNI_ABORT);
    }
    (*env)->ReleaseStringUTFChars(env, outputPath, path);

    if (state == NULL) {
        LOGE("nativeOpenJpegStreamWithExif: open failed");
        return 0;
    }
    encode_worker_reset_for_open();
    return (jlong)(intptr_t)state;
}

/**
 * Writes an RGB_565 strip buffer to the active JPEG stream.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeWriteStreamStrip(
    JNIEnv *env, jclass clazz, jlong handle, jobject directBuffer, jint stripW, jint stripH, jint rowOffset) {
    if (handle == 0 || directBuffer == NULL || stripW <= 0 || stripH <= 0) {
        LOGE("nativeWriteStreamStrip: Invalid parameters");
        return JNI_FALSE;
    }

    if (!encode_worker_wait()) return JNI_FALSE;
    JpegStreamState* state = (JpegStreamState*)(intptr_t)handle;
    uint16_t* pixels = (uint16_t*)(*env)->GetDirectBufferAddress(env, directBuffer);
    if (pixels == NULL) {
        LOGE("nativeWriteStreamStrip: DirectBuffer address is null");
        return JNI_FALSE;
    }

    int ok = native_jpeg_stream_write_strip_rgb565(state, pixels, (int)stripW, (int)stripH, (int)rowOffset);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * Direct alias for native_write_jpeg_strip
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeWriteJpegStrip(
    JNIEnv *env, jclass clazz, jlong handle, jobject directBuffer, jint stripW, jint stripH, jint rowOffset) {
    return Java_com_fujilite_a7r2_ColorEngine_nativeWriteStreamStrip(env, clazz, handle, directBuffer, stripW, stripH, rowOffset);
}

/**
 * Grades a strip buffer with 17^3 3D LUT + Grain + Clarity and pipelines JPEG encoding asynchronously
 * on a dedicated worker thread so strip N+1 decode overlaps with strip N JPEG encoding.
 * Seamlessly supports both ARGB_8888 (full 8-bit precision) and RGB_565.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeGradeAndWriteStreamStrip(
    JNIEnv *env, jclass clazz, jlong handle, jobject directBuffer, jint stripW, jint stripH, jint rowOffset, jint grainStrength, jint clarityStrength) {
    if (handle == 0 || directBuffer == NULL || stripW <= 0 || stripH <= 0) {
        LOGE("nativeGradeAndWriteStreamStrip: Invalid parameters");
        return JNI_FALSE;
    }

    JpegStreamState* state = (JpegStreamState*)(intptr_t)handle;
    jlong cap = (*env)->GetDirectBufferCapacity(env, directBuffer);
    int is_argb = (cap >= (jlong)stripW * stripH * 4) ? 1 : 0;

    int ok = 0;
    if (is_argb) {
        uint32_t* argb_pixels = (uint32_t*)(*env)->GetDirectBufferAddress(env, directBuffer);
        if (argb_pixels == NULL) return JNI_FALSE;

        // Fused 4-Core L2-Cached 3D LUT + 2D AAN FDCT + Quantization + Zero-Copy Async Huffman Emission
        ok = grade_and_encode_strip_parallel(state, 1, argb_pixels, (int)stripW, (int)stripH, (int)rowOffset,
                                             (int)grainStrength, (int)clarityStrength);
    } else {
        uint16_t* rgb_pixels = (uint16_t*)(*env)->GetDirectBufferAddress(env, directBuffer);
        if (rgb_pixels == NULL) return JNI_FALSE;

        // Fused 4-Core L2-Cached 3D LUT + 2D AAN FDCT + Quantization + Zero-Copy Async Huffman Emission
        ok = grade_and_encode_strip_parallel(state, 0, rgb_pixels, (int)stripW, (int)stripH, (int)rowOffset,
                                             (int)grainStrength, (int)clarityStrength);
    }

    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * Closes the active JPEG stream session after joining any in-flight async strip encode.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeCloseJpegStream(
    JNIEnv *env, jclass clazz, jlong handle, jint success) {
    if (handle == 0) return JNI_FALSE;
    int encode_ok = encode_worker_wait();
    if (g_async_enc.buf != NULL) {
        free(g_async_enc.buf);
        g_async_enc.buf = NULL;
        g_async_enc.buf_cap = 0;
    }
    if (g_qstrip_work != NULL) {
        free(g_qstrip_work);
        g_qstrip_work = NULL;
    }
    if (g_qstrip_emit != NULL) {
        free(g_qstrip_emit);
        g_qstrip_emit = NULL;
    }
    g_qstrip_cap = 0;
    JpegStreamState* state = (JpegStreamState*)(intptr_t)handle;
    int ok = native_jpeg_stream_close(state, (success && encode_ok) ? 1 : 0);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * Native C 17^3 3D LUT + Grain + Clarity strip grader for Android Bitmap.getPixels() int[] strips
 * (0xAARRGGBB order). Accelerates fallback grading 10-15x over Dalvik Java without GC critical locks.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeApplyLut17IntStrip(
    JNIEnv *env, jclass clazz, jintArray pixelArray, jint width, jint height,
    jint rowOffset, jint grainStrength, jint clarityStrength) {
    if (pixelArray == NULL || width <= 0 || height <= 0) return JNI_FALSE;
    size_t count = (size_t)width * (size_t)height;
    if ((size_t)(*env)->GetArrayLength(env, pixelArray) < count) return JNI_FALSE;

    pthread_mutex_lock(&g_int_mutex);
    if (g_int_scratch_cap < count) {
        int32_t* grown = (int32_t*)realloc(g_int_scratch, count * sizeof(int32_t));
        if (grown == NULL) {
            pthread_mutex_unlock(&g_int_mutex);
            return JNI_FALSE;
        }
        g_int_scratch = grown;
        g_int_scratch_cap = count;
    }
    (*env)->GetIntArrayRegion(env, pixelArray, 0, (jsize)count, (jint*)g_int_scratch);

    init_static_grain_table();
    int grain_scale = (grainStrength == 2) ? 4 : (grainStrength == 1) ? 2 : 0;

    pthread_mutex_lock(&g_lut_mutex);
    if (grain_scale == 0 && clarityStrength == 0) {
        for (size_t i = 0; i < count; i++) {
            uint32_t px = (uint32_t)g_int_scratch[i];
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t g = (uint8_t)((px >> 8) & 0xFF);
            uint8_t b = (uint8_t)(px & 0xFF);
            uint8_t or_val, og_val, ob_val;
            lut17_interpolate_pixel(&g_lut17, r, g, b, &or_val, &og_val, &ob_val);
            g_int_scratch[i] = (int32_t)(0xFF000000U | ((uint32_t)or_val << 16) | ((uint32_t)og_val << 8) | (uint32_t)ob_val);
        }
    } else {
        size_t idx = 0;
        for (int y = 0; y < height; y++) {
            int global_y = rowOffset + y;
            int grain_y_offset = (global_y & 63) << 6;
            uint32_t rng_state = (uint32_t)(global_y * 1103515245U + 12345U);
            int prev_luma = 128;
            for (int x = 0; x < width; x++) {
                uint32_t px = (uint32_t)g_int_scratch[idx];
                uint8_t r = (uint8_t)((px >> 16) & 0xFF);
                uint8_t g = (uint8_t)((px >> 8) & 0xFF);
                uint8_t b = (uint8_t)(px & 0xFF);
                uint8_t or_val, og_val, ob_val;
                lut17_interpolate_pixel(&g_lut17, r, g, b, &or_val, &og_val, &ob_val);
                int luma = (or_val * 77 + og_val * 150 + ob_val * 29) >> 8;
                int delta = 0;
                if (grain_scale > 0) {
                    rng_state = rng_state * 1103515245U + 12345U;
                    int8_t lcg = (int8_t)((rng_state >> 16) & 0x3F) - 32;
                    int8_t tbl = STATIC_GRAIN_64x64[grain_y_offset | (x & 63)];
                    int midtone = (luma * (255 - luma)) >> 8;
                    delta += (((lcg + tbl) >> 1) * grain_scale * midtone) >> 8;
                }
                if (clarityStrength != 0) {
                    delta += ((luma - prev_luma) * clarityStrength) >> 4;
                }
                prev_luma = luma;
                int dr = clamp_u8_fast(or_val + delta);
                int dg = clamp_u8_fast(og_val + delta);
                int db = clamp_u8_fast(ob_val + delta);
                g_int_scratch[idx++] = (int32_t)(0xFF000000U | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db);
            }
        }
    }
    pthread_mutex_unlock(&g_lut_mutex);

    (*env)->SetIntArrayRegion(env, pixelArray, 0, (jsize)count, (const jint*)g_int_scratch);
    pthread_mutex_unlock(&g_int_mutex);
    return JNI_TRUE;
}

static volatile int g_last_fast_rc = 0;

JNIEXPORT jint JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeGetLastFastRc(
    JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    return (jint)g_last_fast_rc;
}

/**
 * Pure C Zero-JNI-Copy 4-Core Single-Pass Baseline JPEG-to-JPEG Transcoder.
 * Copies the 14.7KB active 17^3 3D LUT under g_lut_mutex so Live View preview is never blocked.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeProcessImageStreamFastScaled(
    JNIEnv *env, jclass clazz, jstring inputPath, jstring outputPath, jint scaleDenom, jint grainStrength, jint clarityStrength) {
    if (inputPath == NULL || outputPath == NULL) {
        g_last_fast_rc = -1;
        return JNI_FALSE;
    }
    const char* in_p = (*env)->GetStringUTFChars(env, inputPath, NULL);
    const char* out_p = (*env)->GetStringUTFChars(env, outputPath, NULL);
    if (in_p == NULL || out_p == NULL) {
        if (in_p != NULL) (*env)->ReleaseStringUTFChars(env, inputPath, in_p);
        if (out_p != NULL) (*env)->ReleaseStringUTFChars(env, outputPath, out_p);
        g_last_fast_rc = -1;
        return JNI_FALSE;
    }

    Lut17Context* lut_copy = (Lut17Context*)malloc(sizeof(Lut17Context));
    if (lut_copy == NULL) {
        (*env)->ReleaseStringUTFChars(env, inputPath, in_p);
        (*env)->ReleaseStringUTFChars(env, outputPath, out_p);
        g_last_fast_rc = -10;
        return JNI_FALSE;
    }
    pthread_mutex_lock(&g_lut_mutex);
    memcpy(lut_copy, &g_lut17, sizeof(Lut17Context));
    pthread_mutex_unlock(&g_lut_mutex);

    int rc = native_fast_transcode_jpeg(lut_copy, in_p, out_p, (int)scaleDenom, 92, (int)grainStrength, (int)clarityStrength);
    g_last_fast_rc = rc;
    int ok = (rc > 0) ? 1 : 0;
    if (!ok) {
        LOGE("native_fast_transcode_jpeg failed (rc=%d) for %s -> %s", rc, in_p, out_p);
        ok = native_process_image_stream_fast(in_p, out_p, (int)grainStrength, (int)clarityStrength);
    } else {
        LOGI("native_fast_transcode_jpeg succeeded (rc=%d) for %s -> %s", rc, in_p, out_p);
    }

    free(lut_copy);
    (*env)->ReleaseStringUTFChars(env, inputPath, in_p);
    (*env)->ReleaseStringUTFChars(env, outputPath, out_p);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * Pure C Zero-JNI-Copy Fast-Path Strip Streaming entrypoint (auto scale: 1/2 for >=7000px, 1:1 otherwise).
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeProcessImageStreamFast(
    JNIEnv *env, jclass clazz, jstring inputPath, jstring outputPath, jint grainStrength, jint clarityStrength) {
    return Java_com_fujilite_a7r2_ColorEngine_nativeProcessImageStreamFastScaled(
        env, clazz, inputPath, outputPath, 0, grainStrength, clarityStrength);
}


