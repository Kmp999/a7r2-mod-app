#include <jni.h>
#include <android/log.h>
#include "color_engine.h"
#include "lut_cube_17.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ColorEngineJNI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ColorEngineJNI", __VA_ARGS__)

static ColorEngineContext g_ctx;
static Lut17Context g_lut17;

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

    jshort* ccm_ptr = (*env)->GetPrimitiveArrayCritical(env, ccmQ8Array, 0);
    jbyte* r_ptr = (*env)->GetPrimitiveArrayCritical(env, toneCurveR, 0);
    jbyte* g_ptr = (*env)->GetPrimitiveArrayCritical(env, toneCurveG, 0);
    jbyte* b_ptr = (*env)->GetPrimitiveArrayCritical(env, toneCurveB, 0);

    if (ccm_ptr && r_ptr && g_ptr && b_ptr) {
        int16_t ccm[9];
        memcpy(ccm, ccm_ptr, sizeof(int16_t) * 9);
        lut17_precompute(&g_lut17, ccm, (uint8_t*)r_ptr, (uint8_t*)g_ptr, (uint8_t*)b_ptr);
        LOGI("17x17x17 3D LUT precomputed successfully");
    }

    if (b_ptr) (*env)->ReleasePrimitiveArrayCritical(env, toneCurveB, b_ptr, 0);
    if (g_ptr) (*env)->ReleasePrimitiveArrayCritical(env, toneCurveG, g_ptr, 0);
    if (r_ptr) (*env)->ReleasePrimitiveArrayCritical(env, toneCurveR, r_ptr, 0);
    if (ccm_ptr) (*env)->ReleasePrimitiveArrayCritical(env, ccmQ8Array, ccm_ptr, 0);
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_applyLut17RGB565(JNIEnv *env, jclass clazz,
                                                                         jshortArray rgb565Array,
                                                                         jint pixelCount) {
    if (rgb565Array == NULL || pixelCount <= 0) {
        return;
    }

    jshort* buf_ptr = (*env)->GetPrimitiveArrayCritical(env, rgb565Array, 0);
    if (buf_ptr == NULL) {
        LOGE("Failed to get critical array pointer for applyLut17RGB565");
        return;
    }

    lut17_process_rgb565(&g_lut17, (uint16_t*)buf_ptr, pixelCount);
    (*env)->ReleasePrimitiveArrayCritical(env, rgb565Array, buf_ptr, 0);
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

    int x, y;
    for (y = 0; y < height; y++) {
        const int8_t* bayerRow = BAYER_4X4[y & 3];
        const int8_t* grainRow = g_grainLut[y & 63];
        int rowOffset = y * width;

        for (x = 0; x < width; x++) {
            int idx = rowOffset + x;
            uint16_t px = buffer[idx];

            // Extract 5-6-5 bits expanded to 8-bit
            uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);

            // 17x17x17 3D LUT Trilinear Interpolation
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

    return JNI_TRUE;
}
