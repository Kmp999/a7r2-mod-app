package com.fujilite.a7r2;

/**
 * Java wrapper for the C++ Color Engine.
 */
public class ColorEngine {
    
    // Preset Constants mapping to C++ PresetId
    public static final int PRESET_PROVIA_LIKE = 0;
    public static final int PRESET_ASTIA_LIKE = 1;
    public static final int PRESET_VELVIA_LIKE = 2;
    public static final int PRESET_CLASSIC_NEG_LIKE = 3;
    public static final int PRESET_ETERNA_LIKE = 4;

    // Mode Constants
    public static final int MODE_MATRIX_LUT = 2;
    public static final int MODE_SAFE_FASTEST = 5;

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
}
