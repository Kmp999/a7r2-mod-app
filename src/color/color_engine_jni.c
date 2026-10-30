#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>
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

    jshort* ccm_ptr = (*env)->GetPrimitiveArrayCritical(env, ccmQ8Array, 0);
    jbyte* r_ptr = (*env)->GetPrimitiveArrayCritical(env, toneCurveR, 0);
    jbyte* g_ptr = (*env)->GetPrimitiveArrayCritical(env, toneCurveG, 0);
    jbyte* b_ptr = (*env)->GetPrimitiveArrayCritical(env, toneCurveB, 0);
    jbyte* base_ptr = (baseLutArray != NULL) ? (*env)->GetPrimitiveArrayCritical(env, baseLutArray, 0) : NULL;

    if (ccm_ptr && r_ptr && g_ptr && b_ptr) {
        int16_t ccm[9];
        memcpy(ccm, ccm_ptr, sizeof(int16_t) * 9);
        lut17_precompute_with_base(&g_lut17, (const uint8_t*)base_ptr, ccm, (uint8_t*)r_ptr, (uint8_t*)g_ptr, (uint8_t*)b_ptr);
        LOGI("17x17x17 3D LUT with base precomputed successfully");
    }

    if (base_ptr) (*env)->ReleasePrimitiveArrayCritical(env, baseLutArray, base_ptr, 0);
    if (b_ptr) (*env)->ReleasePrimitiveArrayCritical(env, toneCurveB, b_ptr, 0);
    if (g_ptr) (*env)->ReleasePrimitiveArrayCritical(env, toneCurveG, g_ptr, 0);
    if (r_ptr) (*env)->ReleasePrimitiveArrayCritical(env, toneCurveR, r_ptr, 0);
    if (ccm_ptr) (*env)->ReleasePrimitiveArrayCritical(env, ccmQ8Array, ccm_ptr, 0);
}

JNIEXPORT void JNICALL Java_com_fujilite_a7r2_ColorEngine_loadLut17Table(JNIEnv *env, jclass clazz, jbyteArray tableArray) {
    if (tableArray == NULL) return;
    jsize len = (*env)->GetArrayLength(env, tableArray);
    if (len < (jsize)sizeof(g_lut17.table)) return;

    jbyte* ptr = (*env)->GetPrimitiveArrayCritical(env, tableArray, 0);
    if (ptr != NULL) {
        memcpy(g_lut17.table, ptr, sizeof(g_lut17.table));
        (*env)->ReleasePrimitiveArrayCritical(env, tableArray, ptr, 0);
        LOGI("17x17x17 3D LUT direct table loaded (%zu bytes)", sizeof(g_lut17.table));
    }
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

/**
 * Fused Real-time Live View Kernel (NV21 to LUT RGB_565).
 * Processes NV21 preview frames, applies 17x17x17 3D LUT, and writes directly
 * to a pre-allocated RGB_565 DirectByteBuffer for Zero-GC rendering on Canvas.
 * Target: < 18ms per frame at 640x480.
 * Supports splitScreenMode: 0 = Full LUT, 1 = Split-Screen 50/50, 2 = Bypass.
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

    jbyte* yuv = (*env)->GetPrimitiveArrayCritical(env, nv21Array, 0);
    if (yuv == NULL) return JNI_FALSE;

    uint16_t* out_rgb = (uint16_t*)(*env)->GetDirectBufferAddress(env, rgb565Direct);
    if (out_rgb == NULL) {
        (*env)->ReleasePrimitiveArrayCritical(env, nv21Array, yuv, 0);
        return JNI_FALSE;
    }

    int frameSize = width * height;
    const uint8_t* yPlane = (const uint8_t*)yuv;
    const uint8_t* uvPlane = (const uint8_t*)(yuv + frameSize);
    int midX = width >> 1;

    int x, y;
    for (y = 0; y < height; y++) {
        int uvOffset = (y >> 1) * width;
        const int8_t* bayerRow = BAYER_4X4[y & 3];
        const int8_t* grainRow = g_grainLut[y & 63];
        int yOffset = y * width;

        for (x = 0; x < width; x++) {
            int yIndex = yOffset + x;

            // Split screen center divider line (Amber Gold #FFAA00 = 0xFDE0 in RGB_565)
            if (splitScreenMode == 1 && (x == midX || x == midX - 1)) {
                out_rgb[yIndex] = 0xFDE0;
                continue;
            }

            int uvIndex = uvOffset + (x & ~1);
            int Y = yPlane[yIndex] - 16;
            if (Y < 0) Y = 0;
            int V = uvPlane[uvIndex] - 128;
            int U = uvPlane[uvIndex + 1] - 128;

            // Integer BT.601 YUV to RGB conversion
            int r0 = (298 * Y + 409 * V + 128) >> 8;
            int g0 = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int b0 = (298 * Y + 516 * U + 128) >> 8;

            uint8_t r = (uint8_t)(r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
            uint8_t g = (uint8_t)(g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
            uint8_t b = (uint8_t)(b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));

            // Determine whether to apply 3D LUT
            // splitScreenMode: 0 = Full LUT, 1 = Split (right half gets LUT), 2 = Bypass (no LUT)
            int applyLut = (splitScreenMode == 0) || (splitScreenMode == 1 && x > midX);

            int dither = bayerRow[x & 3];
            int dr, dg, db;

            if (applyLut) {
                uint8_t or_val, og_val, ob_val;
                lut17_interpolate_pixel(&g_lut17, r, g, b, &or_val, &og_val, &ob_val);
                if (grainLevel > 0) {
                    dither += (grainRow[x & 63] * grainLevel);
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

            // Pack RGB_565
            out_rgb[yIndex] = (uint16_t)(((dr >> 3) << 11) | ((dg >> 2) << 5) | (db >> 3));
        }
    }

    (*env)->ReleasePrimitiveArrayCritical(env, nv21Array, yuv, 0);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeGradeCaptureRGB565(JNIEnv *env, jclass clazz,
                                                                                       jobject directBuffer,
                                                                                       jint width, jint height) {
    return Java_com_fujilite_a7r2_ColorEngine_applyLut17DirectRGB565(env, clazz, directBuffer, width, height);
}

/**
 * Direct Bitmap grading via AndroidBitmap_lockPixels (Requirement 2.2).
 * Operates in-place on 10.6 MP (3984 x 2656) mutable RGB_565 Bitmaps in < 350 ms.
 */
JNIEXPORT jboolean JNICALL Java_com_fujilite_a7r2_ColorEngine_nativeApplyLutAndGrainBitmap(
    JNIEnv *env, jclass clazz, jobject bitmap, jint grainStrength, jint clarityStrength) {
    if (bitmap == NULL) {
        LOGE("nativeApplyLutAndGrainBitmap: Bitmap is null");
        return JNI_FALSE;
    }

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) {
        LOGE("nativeApplyLutAndGrainBitmap: AndroidBitmap_getInfo failed");
        return JNI_FALSE;
    }

    if (info.format != ANDROID_BITMAP_FORMAT_RGB_565) {
        LOGE("nativeApplyLutAndGrainBitmap: Expected RGB_565 format, got %d", info.format);
        return JNI_FALSE;
    }

    void* pixels = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0 || pixels == NULL) {
        LOGE("nativeApplyLutAndGrainBitmap: AndroidBitmap_lockPixels failed");
        return JNI_FALSE;
    }

    lut17_process_rgb565_advanced(&g_lut17, (uint16_t*)pixels, (int)info.width, (int)info.height,
                                  (int)grainStrength, (int)clarityStrength);

    AndroidBitmap_unlockPixels(env, bitmap);
    return JNI_TRUE;
}

