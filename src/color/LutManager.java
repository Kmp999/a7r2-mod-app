package com.fujilite.a7r2;

import android.content.Context;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Map;

/**
 * High-performance 3D LUT Manager for Sony A7R II.
 * Loads 17x17x17 3D LUTs (14,739 bytes) from bundled APK assets or SD Card.
 * Caches loaded tables in native-sized byte arrays to guarantee sub-millisecond preset switching.
 */
public final class LutManager {
    private static final String TAG = "LutManager";
    public static final int LUT_TABLE_BYTES = 17 * 17 * 17 * 3; // 14,739 bytes

    private static final Map<String, byte[]> sCache = new HashMap<String, byte[]>();

    private LutManager() {}

    /**
     * Loads a 17x17x17 3D LUT table for a given asset or SD card file.
     * Returns a 14,739 byte array or null if unavailable.
     */
    public static synchronized byte[] getLutTable(Context context, String filename) {
        if (filename == null || filename.length() == 0) return null;

        if (sCache.containsKey(filename)) {
            return sCache.get(filename);
        }

        byte[] data = null;

        // 1. Try APK Assets (assets/presets/filename)
        if (context != null) {
            try {
                InputStream is = context.getAssets().open("presets/" + filename);
                data = readLutStream(is);
                is.close();
            } catch (Throwable ignored) {
                // Not in assets
            }
        }

        // 2. Try SD Card presets folders
        if (data == null) {
            String[] searchPaths = {
                "/storage/sdcard0/presets/" + filename,
                "/mnt/sdcard/presets/" + filename,
                "/sdcard/presets/" + filename,
                "/sdcard/DCIM/presets/" + filename
            };
            for (String p : searchPaths) {
                File f = new File(p);
                if (f.exists() && f.isFile()) {
                    try {
                        InputStream fis = new FileInputStream(f);
                        data = readLutStream(fis);
                        fis.close();
                        if (data != null) break;
                    } catch (Throwable ignored) {}
                }
            }
        }

        if (data != null && data.length == LUT_TABLE_BYTES) {
            if (isAbpyCameraProfile(filename)) {
                data = normalizeAbpyProfileToSrgb(data);
            }
            sCache.put(filename, data);
            Log.i(TAG, "Loaded and cached 17^3 3D LUT: " + filename);
            return data;
        }

        return null;
    }

    // The 8 bundled Fujifilm camera profiles (abpy/FujifilmCameraProfiles) include Adobe's RAW base
    // tone curve (mapping neutral 128 -> 174). When applied to already tone-mapped Sony sRGB JPEGs,
    // pre-mapping the input through the inverse of Provia's neutral base curve restores exact sRGB
    // exposure while preserving 100% of each film simulation's authentic color science and contrast.
    private static final float[] ABPY_PROVIA_GRAY_Y = {
        0f, 9f, 23f, 46f, 75f, 103f, 130f, 153f, 174f, 192.3f, 208f, 222f, 233f, 242f, 248f, 252f, 255f
    };

    private static boolean isAbpyCameraProfile(String filename) {
        return "Provia.lut".equals(filename)
            || "Velvia.lut".equals(filename)
            || "Astia.lut".equals(filename)
            || "Classic_Chrome.lut".equals(filename)
            || "Classic_Neg.lut".equals(filename)
            || "Pro_Neg_Hi.lut".equals(filename)
            || "Pro_Neg_Ttd.lut".equals(filename)
            || "Eterna.lut".equals(filename);
    }

    private static float invertProviaBaseCurve(float srgbVal) {
        if (srgbVal <= 0f) return 0f;
        if (srgbVal >= 255f) return 255f;
        for (int i = 0; i < 16; i++) {
            float y0 = ABPY_PROVIA_GRAY_Y[i];
            float y1 = ABPY_PROVIA_GRAY_Y[i + 1];
            if (srgbVal <= y1) {
                float t = (y1 > y0) ? ((srgbVal - y0) / (y1 - y0)) : 0f;
                float x0 = (i == 16) ? 255f : (i * 16f);
                float x1 = (i + 1 == 16) ? 255f : ((i + 1) * 16f);
                return x0 + t * (x1 - x0);
            }
        }
        return 255f;
    }

    private static byte[] normalizeAbpyProfileToSrgb(byte[] raw) {
        float[] nodePos = new float[17];
        for (int i = 0; i < 17; i++) {
            float v = (i == 16) ? 255f : (i * 16f);
            nodePos[i] = invertProviaBaseCurve(v) * (16f / 255f);
        }
        byte[] out = new byte[LUT_TABLE_BYTES];
        int dstIdx = 0;
        for (int bi = 0; bi < 17; bi++) {
            float b = nodePos[bi];
            int b0 = Math.min(15, (int) b);
            float bf = b - b0;
            for (int gi = 0; gi < 17; gi++) {
                float g = nodePos[gi];
                int g0 = Math.min(15, (int) g);
                float gf = g - g0;
                for (int ri = 0; ri < 17; ri++) {
                    float r = nodePos[ri];
                    int r0 = Math.min(15, (int) r);
                    float rf = r - r0;
                    float orV = 0f, ogV = 0f, obV = 0f;
                    for (int db = 0; db <= 1; db++) {
                        float wb = (db == 1) ? bf : (1f - bf);
                        int bb = (b0 + db) * 289;
                        for (int dg = 0; dg <= 1; dg++) {
                            float wbg = wb * ((dg == 1) ? gf : (1f - gf));
                            int gg = bb + (g0 + dg) * 17;
                            for (int dr = 0; dr <= 1; dr++) {
                                float w = wbg * ((dr == 1) ? rf : (1f - rf));
                                int off = (gg + r0 + dr) * 3;
                                orV += w * (raw[off] & 0xFF);
                                ogV += w * (raw[off + 1] & 0xFF);
                                obV += w * (raw[off + 2] & 0xFF);
                            }
                        }
                    }
                    int ir = Math.round(orV);
                    int ig = Math.round(ogV);
                    int ib = Math.round(obV);
                    out[dstIdx++] = (byte) (ir < 0 ? 0 : (ir > 255 ? 255 : ir));
                    out[dstIdx++] = (byte) (ig < 0 ? 0 : (ig > 255 ? 255 : ig));
                    out[dstIdx++] = (byte) (ib < 0 ? 0 : (ib > 255 ? 255 : ib));
                }
            }
        }
        return out;
    }

    private static byte[] readLutStream(InputStream is) {
        if (is == null) return null;
        try {
            byte[] buf = new byte[LUT_TABLE_BYTES];
            int totalRead = 0;
            while (totalRead < LUT_TABLE_BYTES) {
                int read = is.read(buf, totalRead, LUT_TABLE_BYTES - totalRead);
                if (read <= 0) break;
                totalRead += read;
            }
            if (totalRead == LUT_TABLE_BYTES) {
                return buf;
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed reading LUT stream: " + t.getMessage());
        }
        return null;
    }

    /**
     * Clears cached LUT tables to reclaim memory if needed.
     */
    public static synchronized void clearCache() {
        sCache.clear();
    }
}
