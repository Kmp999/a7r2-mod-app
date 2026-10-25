#include <jni.h>
#include <android/log.h>
#include "color_engine.h"
#include "lut_cube_17.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ColorEngineJNI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ColorEngineJNI", __VA_ARGS__)

static ColorEngineContext g_ctx;
static Lut17Context g_lut17;

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_initEngine(JNIEnv *env, jclass clazz, jint presetId, jint mode) {
    color_engine_init(&g_ctx, (PresetId)presetId, mode);
    LOGI("Engine initialized with preset %d, mode %d", presetId, mode);
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_processNV21(JNIEnv *env, jclass clazz, jbyteArray nv21Array, jint width, jint height) {
    if (nv21Array == NULL || width <= 0 || height <= 0) {
        LOGE("Invalid arguments provided to processNV21");
        return;
    }

    // Obtain direct pointer to avoid copying (Critical for <500ms budget)
    jbyte* buffer_ptr = (*env)->GetPrimitiveArrayCritical(env, nv21Array, 0);
    if (buffer_ptr == NULL) {
        LOGE("Failed to get critical array pointer");
        return;
    }

    // Fixed-point in-place YUV processing
    color_engine_process_nv21(&g_ctx, (uint8_t*)buffer_ptr, width, height);

    // Release pointer (0 means copy back changes if it wasn't pinned, but GetPrimitiveArrayCritical usually pins)
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
