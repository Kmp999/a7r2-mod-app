#include <jni.h>
#include <android/log.h>
#include "color_engine.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ColorEngineJNI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ColorEngineJNI", __VA_ARGS__)

static ColorEngineContext g_ctx;

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
