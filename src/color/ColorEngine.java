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
     * Call this once during app startup or when changing presets.
     */
    public static native void initEngine(int presetId, int mode);

    /**
     * Processes an NV21 byte array in-place.
     * WARNING: Runs synchronously. Call from a background thread to prevent UI blocks.
     * 
     * @param nv21Array The YUV data buffer from Camera.PreviewCallback
     * @param width Width of the image
     * @param height Height of the image
     */
    public static native void processNV21(byte[] nv21Array, int width, int height);

    /**
     * Processes a packed RGB_565 buffer (one short per pixel) in-place.
     * Used by the full-resolution capture path after JPEG decode.
     */
    public static native void processRGB565(short[] rgb565, int width, int height);
}
