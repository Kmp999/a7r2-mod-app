package com.fujilite.a7r2;

import android.util.Log;

/**
 * Java wrapper for the C++ Color Engine.
 */
public class ColorEngine {

    private static final String TAG = "ColorEngine";

    // Preset Constants mapping to C++ PresetId
    public static final int PRESET_PROVIA_LIKE = 0;
    public static final int PRESET_ASTIA_LIKE = 1;
    public static final int PRESET_VELVIA_LIKE = 2;
    public static final int PRESET_CLASSIC_NEG_LIKE = 3;
    public static final int PRESET_ETERNA_LIKE = 4;

    // Mode Constants
    public static final int MODE_MATRIX_LUT = 2;
    public static final int MODE_SAFE_FASTEST = 5;

    // The native library is loaded here (not in the Activity) and failures are
    // non-fatal: on the A7R II an UnsatisfiedLinkError in an Activity static
    // initializer crashes the app on launch (single vibration, bounce to menu).
    private static boolean sNativeAvailable = false;

    static {
        try {
            System.loadLibrary("color_engine_jni");
            sNativeAvailable = true;
        } catch (Throwable t) {
            Log.e(TAG, "Native color engine unavailable; running in passthrough mode", t);
            sNativeAvailable = false;
        }
    }

    public static boolean isNativeAvailable() {
        return sNativeAvailable;
    }

    /** Initializes the native engine; safe no-op if the library failed to load. */
    public static void initEngineSafe(int presetId, int mode) {
        if (!sNativeAvailable) return;
        try {
            initEngine(presetId, mode);
        } catch (Throwable t) {
            Log.e(TAG, "initEngine failed; disabling native engine", t);
            sNativeAvailable = false;
        }
    }

    /** Precomputes 17x17x17 3D LUT in native memory from recipe parameters. */
    public static boolean precomputeLut17Safe(short[] ccmQ8, byte[] rCurve, byte[] gCurve, byte[] bCurve) {
        if (!sNativeAvailable) return false;
        try {
            precomputeLut17(ccmQ8, rCurve, gCurve, bCurve);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "precomputeLut17 failed", t);
            return false;
        }
    }

    /** Applies precomputed 17x17x17 3D LUT to packed RGB_565 buffer in-place. */
    public static boolean applyLut17RGB565Safe(short[] rgb565, int pixelCount) {
        if (!sNativeAvailable) return false;
        try {
            applyLut17RGB565(rgb565, pixelCount);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "applyLut17RGB565 failed", t);
            return false;
        }
    }

    /** Processes NV21 in-place; returns true if native processing ran. */
    public static boolean processNV21Safe(byte[] nv21Array, int width, int height) {
        if (!sNativeAvailable) return false;
        try {
            processNV21(nv21Array, width, height);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "processNV21 failed", t);
            return false;
        }
    }

    /** Processes a packed RGB_565 buffer in-place; returns true if native processing ran. */
    public static boolean processRGB565Safe(short[] rgb565, int width, int height) {
        if (!sNativeAvailable) return false;
        try {
            processRGB565(rgb565, width, height);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "processRGB565 failed", t);
            return false;
        }
    }

    /**
     * Initializes the native engine with precomputed LUTs and Matrices.
     */
    public static native void initEngine(int presetId, int mode);

    /**
     * Processes an NV21 byte array in-place.
     */
    public static native void processNV21(byte[] nv21Array, int width, int height);

    /**
     * Processes a packed RGB_565 buffer (one short per pixel) in-place.
     */
    public static native void processRGB565(short[] rgb565, int width, int height);

    /**
     * Precomputes 17x17x17 3D LUT from matrix and tone curves.
     */
    public static native void precomputeLut17(short[] ccmQ8, byte[] toneCurveR, byte[] toneCurveG, byte[] toneCurveB);

    /**
     * Applies 17x17x17 3D LUT in-place to RGB_565 buffer.
     */
    public static native void applyLut17RGB565(short[] rgb565, int pixelCount);

    /**
     * Applies 17x17x17 3D LUT with 4x4 Bayer Dithering & Grain directly to a Direct ByteBuffer in-place.
     * Zero-Copy: Never locks the Dalvik Garbage Collector.
     */
    public static boolean applyLut17DirectRGB565Safe(java.nio.ByteBuffer directBuffer, int width, int height) {
        if (!sNativeAvailable || directBuffer == null || !directBuffer.isDirect()) return false;
        try {
            return applyLut17DirectRGB565(directBuffer, width, height);
        } catch (Throwable t) {
            Log.e(TAG, "applyLut17DirectRGB565 failed", t);
            return false;
        }
    }

    public static native boolean applyLut17DirectRGB565(java.nio.ByteBuffer directBuffer, int width, int height);

    /**
     * Converts NV21 camera preview frames to RGB_565, applying the 3D LUT and Grain in a single pass.
     * Outputs directly to a Zero-GC DirectByteBuffer.
     */
    public static boolean gradePreviewNV21toRGB565Safe(byte[] nv21, java.nio.ByteBuffer rgb565Direct, int width, int height, int grain) {
        if (!sNativeAvailable || nv21 == null || rgb565Direct == null || !rgb565Direct.isDirect()) return false;
        try {
            return nativeGradePreviewNV21toRGB565(nv21, rgb565Direct, width, height, grain);
        } catch (Throwable t) {
            Log.e(TAG, "nativeGradePreviewNV21toRGB565 failed", t);
            return false;
        }
    }

    public static native boolean nativeGradePreviewNV21toRGB565(byte[] nv21Array, java.nio.ByteBuffer rgb565Direct, int width, int height, int grainStrength);
}
