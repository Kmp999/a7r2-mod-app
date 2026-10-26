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
            sCache.put(filename, data);
            Log.i(TAG, "Loaded and cached 17^3 3D LUT: " + filename);
            return data;
        }

        return null;
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
