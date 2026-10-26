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
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Set;

/**
 * 3-Tier StorageResolver for Sony PMCA / ScalarOS (ILCE-7RM2 / A7R II).
 *
 * Tier 1: Sony Scalar AvindexStore.getExternalMediaMountedPath(id) reflection.
 * Tier 2: Kernel /proc/mounts scanner (excluding /tmp*, /system, /data, /cache).
 * Tier 3: Hardcoded Sony Alpha fallback list (/storage/sdcard0, /mnt/sd0, etc).
 *
 * Enforces DCF 8.3 filenames (FUJI0001.JPG) and 4-tier AvindexStore cataloging.
 */
public final class StorageResolver {
    private static final String TAG = "StorageResolver";

    private static volatile File sCachedWritableDcimDir = null;
    private static volatile String sLastDiagnostic = "Not initialized";
    private static int sFileSequence = 1;

    private StorageResolver() {}

    /**
     * Resolves the verified writable DCIM directory on the physical SD card of Sony A7R II.
     */
    public static synchronized File findSonySdCardMount() {
        if (sCachedWritableDcimDir != null && sCachedWritableDcimDir.exists()) {
            if (verifyWritable(sCachedWritableDcimDir)) {
                return sCachedWritableDcimDir;
            }
            sCachedWritableDcimDir = null;
        }

        Set<String> candidatePaths = new LinkedHashSet<String>();

        // --------------------------------------------------------------------
        // Tầng 1: Sử dụng Reflection gọi Sony Scalar AvindexStore API chính thức
        // --------------------------------------------------------------------
        try {
            Class<?> avindexClass = Class.forName("com.sony.scalar.provider.AvindexStore");
            Method getMediaIdsMethod = avindexClass.getMethod("getExternalMediaIds");
            String[] mediaIds = (String[]) getMediaIdsMethod.invoke(null);
            if (mediaIds != null && mediaIds.length > 0) {
                Method getMountedPathMethod = avindexClass.getMethod("getExternalMediaMountedPath", String.class);
                for (String id : mediaIds) {
                    String mountedPath = (String) getMountedPathMethod.invoke(null, id);
                    if (mountedPath != null && mountedPath.length() > 0) {
                        candidatePaths.add(mountedPath + "/DCIM/100MSDCF");
                        candidatePaths.add(mountedPath + "/DCIM");
                        candidatePaths.add(mountedPath);
                        Log.i(TAG, "Tier 1: Found Avindex mounted media path: " + mountedPath);
                    }
                }
            }
        } catch (Throwable t) {
            Log.d(TAG, "Tier 1: Sony AvindexStore media reflection not available: " + t.getMessage());
        }

        // --------------------------------------------------------------------
        // Tầng 2: Quét toàn bộ /proc/mounts của Linux Kernel
        // --------------------------------------------------------------------
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

                    // Bỏ qua các phân vùng hệ thống và thư mục tạm ảo nội bộ
                    // (/tmp/setting/lens là nơi lưu calibration lens, không phải thẻ nhớ!)
                    if (mountPoint.equals("/system") || mountPoint.equals("/data")
                        || mountPoint.equals("/cache") || mountPoint.equals("/proc")
                        || mountPoint.equals("/sys") || mountPoint.equals("/dev")
                        || mountPoint.startsWith("/tmp")) {
                        continue;
                    }

                    // Nhận diện phân vùng có cờ ghi (rw) và liên quan đến thẻ nhớ
                    if (options.startsWith("rw") || options.contains(",rw")) {
                        if (fsType.contains("fat") || fsType.contains("fuse") || fsType.contains("ntfs")
                            || fsType.contains("sd") || mountPoint.contains("sd")
                            || mountPoint.contains("storage") || mountPoint.contains("media")) {
                            candidatePaths.add(mountPoint + "/DCIM/100MSDCF");
                            candidatePaths.add(mountPoint + "/DCIM");
                            candidatePaths.add(mountPoint);
                        }
                    }
                }
            }
        } catch (Throwable ignored) {
        } finally {
            if (reader != null) {
                try { reader.close(); } catch (Throwable ignored) {}
            }
        }

        // --------------------------------------------------------------------
        // Tầng 3: Các đường dẫn chuẩn của Sony ScalarOS và Android
        // --------------------------------------------------------------------
        candidatePaths.add("/storage/sdcard0/DCIM/100MSDCF");
        candidatePaths.add("/mnt/sd0/DCIM/100MSDCF");
        candidatePaths.add("/mnt/media_rw/sdcard0/DCIM/100MSDCF");
        candidatePaths.add("/mnt/ext_sd/DCIM/100MSDCF");
        candidatePaths.add("/mnt/sdcard/DCIM/100MSDCF");
        if (Environment.getExternalStorageDirectory() != null) {
            candidatePaths.add(new File(Environment.getExternalStorageDirectory(), "DCIM/100MSDCF").getAbsolutePath());
        }

        // --------------------------------------------------------------------
        // Kiểm tra quyền ghi thực tế bằng file test .probe
        // --------------------------------------------------------------------
        for (String path : candidatePaths) {
            File dir = new File(path);
            if (!dir.exists()) {
                try { dir.mkdirs(); } catch (Throwable ignored) {}
            }
            if (dir.exists() && dir.isDirectory()) {
                if (verifyWritable(dir)) {
                    sCachedWritableDcimDir = dir;
                    sLastDiagnostic = "OK: " + dir.getAbsolutePath();
                    Log.i(TAG, "Selected verified writable SD directory: " + dir.getAbsolutePath());
                    return dir;
                }
            }
        }

        sLastDiagnostic = "FAIL: No writable mount among " + candidatePaths.toString();
        Log.e(TAG, sLastDiagnostic);
        return null;
    }

    /**
     * Performs an actual physical file write and delete test.
     */
    private static boolean verifyWritable(File dir) {
        if (dir == null || !dir.exists()) return false;
        File testFile = new File(dir, ".probe_" + System.currentTimeMillis());
        FileOutputStream fos = null;
        try {
            fos = new FileOutputStream(testFile);
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
     */
    public static synchronized String generateDcfFilename(File dir) {
        if (dir == null) return "FUJI0001.JPG";

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
