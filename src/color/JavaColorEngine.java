package com.fujilite.a7r2;

/**
 * Pure-Java color engine. Mirrors the native fixed-point matrix + S-curve LUT so
 * the app can grade images without loading any JNI library. On Sony PMCA cameras
 * a failed native dlopen can abort the whole process, so this keeps the app
 * fully functional with zero native dependency.
 */
public final class JavaColorEngine {

    // Q8.8 3x3 matrices (256 == 1.0), matching src/color/presets/*.c
    private static final int[][][] MATRICES = {
        // Provia-like
        {{266, -5, -5}, {-5, 266, -5}, {-5, -5, 266}},
        // Astia-like
        {{248, 6, 2}, {2, 254, 0}, {0, 4, 252}},
        // Velvia-like
        {{290, -20, -14}, {-16, 295, -23}, {-10, -25, 291}},
        // Classic Neg-like
        {{230, 16, 10}, {-12, 270, -2}, {10, -15, 261}},
        // Eterna-like
        {{210, 25, 21}, {20, 215, 21}, {20, 24, 212}},
    };

    // S-curve params {shadow_lift, highlight_rolloff, contrast_boost}
    private static final int[][] LUT_PARAMS = {
        {0, 0, 15},     // Provia
        {5, -10, 8},    // Astia
        {-15, 10, 40},  // Velvia
        {15, 20, 25},   // Classic Neg
        {20, 30, -10},  // Eterna
    };

    private int[][] matrix;
    private int[] lut;

    public void init(int presetId) {
        int id = (presetId < 0 || presetId >= MATRICES.length) ? 0 : presetId;
        this.matrix = MATRICES[id];
        this.lut = generateScurve(LUT_PARAMS[id][0], LUT_PARAMS[id][1], LUT_PARAMS[id][2]);
    }

    private static int[] generateScurve(int shadowLift, int highlightRolloff, int contrastBoost) {
        int[] curve = new int[256];
        double contrastFactor = 1.0 + ((double) contrastBoost / 100.0);
        for (int i = 0; i < 256; i++) {
            double x = i / 255.0;
            double centered = (x - 0.5) * contrastFactor;
            double s = 1.0 / (1.0 + Math.exp(-6.0 * centered));
            if (x < 0.3) {
                s += ((double) shadowLift / 255.0) * ((0.3 - x) / 0.3);
            }
            if (x > 0.7) {
                s -= ((double) highlightRolloff / 255.0) * ((x - 0.7) / 0.3);
            }
            if (s < 0.0) s = 0.0;
            if (s > 1.0) s = 1.0;
            curve[i] = (int) (s * 255.0 + 0.5);
        }
        return curve;
    }

    private static int clamp(int v) {
        return v < 0 ? 0 : (v > 255 ? 255 : v);
    }

    /** Processes a packed RGB_565 buffer (one short per pixel) in-place. */
    public void processRGB565(short[] px, int width, int height) {
        if (matrix == null || lut == null) {
            init(0);
        }
        final int[][] m = matrix;
        final int[] l = lut;
        int total = width * height;
        for (int i = 0; i < total; i++) {
            int p = px[i] & 0xFFFF;
            int r = (((p >> 11) & 0x1F) * 255) / 31;
            int g = (((p >> 5) & 0x3F) * 255) / 63;
            int b = (p & 0x1F) * 255 / 31;

            int rout = (m[0][0] * r + m[0][1] * g + m[0][2] * b + 128) >> 8;
            int gout = (m[1][0] * r + m[1][1] * g + m[1][2] * b + 128) >> 8;
            int bout = (m[2][0] * r + m[2][1] * g + m[2][2] * b + 128) >> 8;

            r = l[clamp(rout)];
            g = l[clamp(gout)];
            b = l[clamp(bout)];

            px[i] = (short) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}
