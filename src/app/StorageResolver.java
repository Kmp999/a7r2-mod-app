package com.fujilite.a7r2;

import android.content.Context;
import android.content.Intent;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Environment;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * StorageResolver for Sony PMCA / ScalarOS (A7R II).
 *
 * Resolves the true physical SD card by:
 *  1. Parsing /proc/mounts to find mounted FAT32/exFAT partitions (e.g. /mnt/sd0).
 *  2. Performing real physical write-probes (creating, syncing, and deleting a test file).
 *  3. Enforcing DCF 8.3 naming (FUJI0001.JPG) required by Sony's AvindexStore catalog.
 *  4. 4-tier registration with Sony's media indexing service for immediate camera playback.
 */
public final class StorageResolver {
    private static final String TAG = "StorageResolver";

    private static volatile File sCachedWritableDcimDir = null;
    private static volatile String sLastDiagnostic = "Not initialized";
    private static int sFileSequence = 1;

    private StorageResolver() {}

    /**
     * Resolves and verifies the real writable DCIM directory on the physical SD card.
     */
    public static synchronized File findSonySdCardMount() {
        if (sCachedWritableDcimDir != null && sCachedWritableDcimDir.exists()) {
            // Fast verify that cached directory is still writable
            if (verifyWritable(sCachedWritableDcimDir)) {
                return sCachedWritableDcimDir;
            }
            sCachedWritableDcimDir = null;
        }

        List<String> candidates = new ArrayList<String>();
        StringBuilder diagBuilder = new StringBuilder();

        // 1. Parse /proc/mounts to find actual mounted vfat / exfat / texfat / sdcardfs partitions
        BufferedReader reader = null;
        try {
            reader = new BufferedReader(new FileReader("/proc/mounts"));
            String line;
            while ((line = reader.readLine()) != null) {
                String[] parts = line.split("\\s+");
                if (parts.length >= 4) {
                    String mountPoint = parts[1];
                    String fsType = parts[2].toLowerCase(Locale.US);
                    String options = parts[3].toLowerCase(Locale.US);

                    // Look for FAT/exFAT SD card partitions mounted Read-Write
                    boolean isFsCandidate = fsType.contains("fat") || fsType.contains("sdcard")
                                         || fsType.contains("fuse") || fsType.contains("ntfs")
                                         || mountPoint.contains("sd");
                    boolean isRw = options.startsWith("rw") || options.contains(",rw");
                    boolean isExcluded = mountPoint.equals("/system") || mountPoint.equals("/data")
                                      || mountPoint.equals("/cache") || mountPoint.equals("/proc")
                                      || mountPoint.equals("/sys") || mountPoint.equals("/dev");

                    if (isFsCandidate && isRw && !isExcluded) {
                        if (!candidates.contains(mountPoint)) {
                            candidates.add(mountPoint);
                        }
                    }
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed to parse /proc/mounts", t);
        } finally {
            if (reader != null) {
                try { reader.close(); } catch (Throwable ignored) {}
            }
        }

        // 2. Add Sony PMCA and Android standard fallback mount points
        String[] standardFallbacks = {
            "/mnt/sd0",                 // Primary Sony Alpha physical SD slot
            "/mnt/sd1",                 // Secondary slot
            "/mnt/ext_sd",
            "/storage/sdcard0",
            "/mnt/sdcard",
            "/mnt/media_rw/sdcard0"
        };
        for (String fb : standardFallbacks) {
            if (!candidates.contains(fb)) {
                candidates.add(fb);
            }
        }
        if (Environment.getExternalStorageDirectory() != null) {
            String extPath = Environment.getExternalStorageDirectory().getAbsolutePath();
            if (!candidates.contains(extPath)) {
                candidates.add(extPath);
            }
        }

        diagBuilder.append("Probed ").append(candidates.size()).append(" mounts: ");

        // 3. Probe each candidate for writable DCIM directory
        for (String rootPath : candidates) {
            File root = new File(rootPath);
            if (!root.exists() || !root.isDirectory()) continue;

            File[] targets = new File[] {
                new File(root, "DCIM/100MSDCF"),
                new File(root, "DCIM/101MSDCF"),
                new File(root, "DCIM"),
                root
            };

            for (File dir : targets) {
                if (!dir.exists()) {
                    try { dir.mkdirs(); } catch (Throwable ignored) {}
                }
                if (dir.exists() && dir.isDirectory()) {
                    if (verifyWritable(dir)) {
                        sCachedWritableDcimDir = dir;
                        sLastDiagnostic = "OK: " + dir.getAbsolutePath();
                        Log.i(TAG, "Verified writable SD mount: " + dir.getAbsolutePath());
                        return dir;
                    }
                }
            }
        }

        sLastDiagnostic = "FAIL: No writable mount among " + candidates.toString();
        Log.e(TAG, sLastDiagnostic);
        return null;
    }

    /**
     * Performs an actual physical file write and delete test.
     */
    private static boolean verifyWritable(File dir) {
        if (dir == null || !dir.exists()) return false;
        File testFile = new File(dir, ".fuji_probe_" + (System.currentTimeMillis() % 1000));
        FileOutputStream fos = null;
        try {
            fos = new FileOutputStream(testFile);
            // Write standard JPEG SOI/EOI magic markers
            fos.write(new byte[]{ (byte)0xFF, (byte)0xD8, (byte)0xFF, (byte)0xD9 });
            fos.flush();
            try {
                fos.getFD().sync();
            } catch (Throwable ignored) {}
            fos.close();
            fos = null;
            testFile.delete();
            return true;
        } catch (Throwable t) {
            return false;
        } finally {
            if (fos != null) {
                try { fos.close(); } catch (Throwable ignored) {}
            }
            if (testFile.exists()) {
                testFile.delete();
            }
        }
    }

    /**
     * Generates a strict DCF-compliant 8.3 filename (e.g. FUJI0001.JPG).
     * Required by Sony's AvindexStore catalog database.
     */
    public static synchronized String generateDcfFilename(File dir) {
        if (dir == null) return "FUJI0001.JPG";

        // Find next non-colliding filename
        for (int i = 0; i < 9999; i++) {
            int num = (sFileSequence++) % 10000;
            if (num == 0) num = 1;
            String name = String.format(Locale.US, "FUJI%04d.JPG", num);
            File candidate = new File(dir, name);
            if (!candidate.exists()) {
                return name;
            }
        }
        return String.format(Locale.US, "FUJI%04d.JPG", (int)(System.currentTimeMillis() % 9000) + 1000);
    }

    /**
     * Registers a freshly written JPEG with Sony AvindexStore and standard Android MediaScanner.
     */
    public static void registerWithAvindex(final Context context, final String filePath) {
        if (filePath == null) return;
        final File file = new File(filePath);
        if (!file.exists()) return;

        // 1. Android MediaScannerConnection
        if (context != null) {
            try {
                MediaScannerConnection.scanFile(
                    context.getApplicationContext(),
                    new String[]{ filePath },
                    new String[]{ "image/jpeg" },
                    null
                );
            } catch (Throwable t) {
                Log.w(TAG, "MediaScannerConnection failed", t);
            }

            // 2. Android MediaScanner Broadcast
            try {
                Intent scanIntent = new Intent(Intent.ACTION_MEDIA_SCANNER_SCAN_FILE, Uri.fromFile(file));
                context.sendBroadcast(scanIntent);
            } catch (Throwable ignored) {}

            // 3. Sony Scalar Avindex Broadcast
            try {
                Intent sonyIntent = new Intent("com.sony.scalar.providers.avindex.ACTION_DATABASE_UPDATED");
                context.sendBroadcast(sonyIntent);
            } catch (Throwable ignored) {}
        }

        // 4. Sony Scalar AvindexStore API Reflection
        try {
            Class<?> cls = Class.forName("com.sony.scalar.provider.AvindexStore$Images$Media");
            for (Method m : cls.getMethods()) {
                if ("notifyImageCreated".equals(m.getName())) {
                    Class<?>[] pTypes = m.getParameterTypes();
                    if (pTypes.length == 1 && pTypes[0] == String.class) {
                        m.invoke(null, filePath);
                        Log.i(TAG, "AvindexStore.notifyImageCreated(path) succeeded");
                        break;
                    } else if (pTypes.length == 2 && context != null) {
                        if (Context.class.isAssignableFrom(pTypes[0]) && pTypes[1] == String.class) {
                            m.invoke(null, context, filePath);
                            Log.i(TAG, "AvindexStore.notifyImageCreated(context, path) succeeded");
                            break;
                        } else if (android.content.ContentResolver.class.isAssignableFrom(pTypes[0]) && pTypes[1] == String.class) {
                            m.invoke(null, context.getContentResolver(), filePath);
                            Log.i(TAG, "AvindexStore.notifyImageCreated(cr, path) succeeded");
                            break;
                        }
                    }
                }
            }
        } catch (Throwable t) {
            Log.d(TAG, "AvindexStore reflection not available: " + t.getMessage());
        }
    }

    public static String getLastDiagnostic() {
        return sLastDiagnostic;
    }
}
