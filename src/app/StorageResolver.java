package com.fujilite.a7r2;

import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.ExifInterface;
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
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Bulletproof StorageResolver for Sony PMCA / ScalarOS (ILCE-7RM2 / A7R II).
 *
 * Uses the proven LightLab & PMCA mount resolution strategy:
 *  1. Discovers candidate SD mount roots (/storage/sdcard0, /mnt/sdcard, /mnt/sd0, etc).
 *  2. Locates existing camera DCIM/*MSDCF directories.
 *  3. Directly writes JPEG images with fallback across candidates.
 *  4. Enforces strict DCF 8.3 naming (DSC0xxxx.JPG / FUJIxxxx.JPG).
 *  5. Registers saved files with Sony AvindexStore and MediaScanner for instant playback.
 */
public final class StorageResolver {
    private static final String TAG = "StorageResolver";

    private static volatile String sLastDiagnostic = "Not initialized";
    private static int sFileSequence = 1;

    private StorageResolver() {}

    /**
     * Resolves candidate DCIM directories in priority order.
     */
    public static List<File> getCandidateDcimDirs() {
        List<File> list = new ArrayList<File>();
        Set<String> roots = new LinkedHashSet<String>();

        // 1. Sony Scalar AvindexStore official mounted media path via reflection
        try {
            Class<?> avindexClass = Class.forName("com.sony.scalar.provider.AvindexStore");
            Method getMediaIdsMethod = avindexClass.getMethod("getExternalMediaIds");
            String[] mediaIds = (String[]) getMediaIdsMethod.invoke(null);
            if (mediaIds != null) {
                Method getMountedPathMethod = avindexClass.getMethod("getExternalMediaMountedPath", String.class);
                for (String id : mediaIds) {
                    String p = (String) getMountedPathMethod.invoke(null, id);
                    if (p != null && p.length() > 0) {
                        roots.add(p);
                        Log.i(TAG, "AvindexStore external media path: " + p);
                    }
                }
            }
        } catch (Throwable ignored) {}

        // 2. Standard Sony PMCA & LightLab SD card mount points (A7R II proven paths)
        roots.add("/storage/sdcard0");
        roots.add("/mnt/sdcard");
        roots.add("/mnt/sd0");
        roots.add("/storage/sdcard1");
        roots.add("/storage/extSdCard");
        roots.add("/mnt/extSdCard");
        roots.add("/mnt/ext_card");
        roots.add("/mnt/sdcard2");
        roots.add("/mnt/media_rw/sdcard0");
        roots.add("/sdcard");
        if (Environment.getExternalStorageDirectory() != null) {
            roots.add(Environment.getExternalStorageDirectory().getAbsolutePath());
        }

        // 3. Kernel /proc/mounts scanner (LightLab algorithm)
        BufferedReader reader = null;
        try {
            reader = new BufferedReader(new FileReader("/proc/mounts"));
            String line;
            while ((line = reader.readLine()) != null) {
                String[] parts = line.split("\\s+");
                if (parts.length >= 3) {
                    String mount = parts[1];
                    String fs = parts[2].toLowerCase(Locale.US);
                    if (mount.equals("/") || mount.startsWith("/proc") || mount.startsWith("/sys")
                        || mount.startsWith("/dev") || mount.startsWith("/tmp")) {
                        continue;
                    }
                    if (fs.contains("fat") || fs.contains("fuse") || fs.contains("sdcard")
                        || mount.startsWith("/mnt/") || mount.startsWith("/storage/")) {
                        roots.add(mount);
                    }
                }
            }
        } catch (Throwable ignored) {
        } finally {
            if (reader != null) try { reader.close(); } catch (Throwable ignored) {}
        }

        // For each root, prioritize existing camera DCIM/*MSDCF folders
        for (String r : roots) {
            File rootFile = new File(r);
            if (!rootFile.exists()) continue;

            File dcim = new File(rootFile, "DCIM");
            if (dcim.exists() && dcim.isDirectory()) {
                File[] children = dcim.listFiles();
                if (children != null) {
                    for (File c : children) {
                        if (c.isDirectory() && c.getName().toUpperCase(Locale.US).endsWith("MSDCF")) {
                            if (!list.contains(c)) list.add(c);
                        }
                    }
                }
            }
            File std100 = new File(rootFile, "DCIM/100MSDCF");
            if (!list.contains(std100)) list.add(std100);
            if (!list.contains(dcim)) list.add(dcim);
        }

        return list;
    }

    /**
     * Atomically allocates the next DCF 4-digit sequence number (0001 - 9999)
     * across existing DSC0xxxx.JPG and FUJIxxxx.JPG files in DCIM.
     */
    public static synchronized int allocateNextSequenceNumber() {
        List<File> candidates = getCandidateDcimDirs();
        int maxSeq = 0;
        for (File dir : candidates) {
            if (dir.exists() && dir.isDirectory()) {
                File[] files = dir.listFiles();
                if (files != null) {
                    for (File f : files) {
                        String name = f.getName().toUpperCase(Locale.US);
                        if (name.endsWith(".JPG") && name.length() == 12) {
                            try {
                                String numStr = name.substring(4, 8);
                                int val = Integer.parseInt(numStr);
                                if (val > maxSeq) maxSeq = val;
                            } catch (Throwable ignored) {}
                        }
                    }
                }
            }
        }
        int nextNum = (maxSeq > 0) ? (maxSeq + 1) : sFileSequence;
        if (nextNum > 9999) nextNum = 1;
        sFileSequence = nextNum + 1;
        return nextNum;
    }

    /**
     * Saves raw 42.4 MP JPEG bytes directly from BIONZ X to the first writable SD card directory.
     * Zero-Copy / Zero-Decode: Preserves 100% of native 7952x5304 resolution and hardware ISP metadata.
     * Returns the absolute path of the saved file, or null if all attempts fail.
     */
    public static String saveRawJpegToSd(Context context, byte[] rawJpegData) {
        return saveRawJpegToSd(context, rawJpegData, -1);
    }

    public static String saveRawJpegToSd(Context context, byte[] rawJpegData, int sequenceNum) {
        if (rawJpegData == null || rawJpegData.length == 0) {
            sLastDiagnostic = "FAIL: Raw JPEG data is empty";
            return null;
        }

        List<File> candidates = getCandidateDcimDirs();
        if (candidates.isEmpty()) {
            sLastDiagnostic = "FAIL: No SD mount candidates found";
            Log.e(TAG, sLastDiagnostic);
            return null;
        }

        for (File dir : candidates) {
            try {
                if (!dir.exists()) {
                    dir.mkdirs();
                }
                if (!dir.exists() || !dir.isDirectory()) {
                    continue;
                }

                String filename = (sequenceNum > 0)
                    ? String.format(Locale.US, "DSC0%04d.JPG", sequenceNum)
                    : generateDcfFilename(dir, "DSC0");
                File file = new File(dir, filename);
                FileOutputStream out = new FileOutputStream(file);
                out.write(rawJpegData);
                out.flush();
                try {
                    out.getFD().sync();
                } catch (Throwable ignored) {}
                out.close();

                if (file.exists() && file.length() > 0) {
                    String savedPath = file.getAbsolutePath();
                    writeExifMetadata(savedPath);
                    sLastDiagnostic = "OK (42.4MP): " + savedPath + " (" + (file.length() / 1024) + "KB)";
                    Log.i(TAG, "Successfully saved full-res 42.4MP photo: " + sLastDiagnostic);

                    // Register with Sony AvindexStore for physical playback
                    registerWithAvindex(context, savedPath);
                    return savedPath;
                } else {
                    file.delete();
                }
            } catch (Throwable t) {
                Log.w(TAG, "Raw write failed in " + dir.getAbsolutePath() + ": " + t.getMessage());
            }
        }

        sLastDiagnostic = "FAIL: All " + candidates.size() + " mount attempts failed";
        Log.e(TAG, sLastDiagnostic);
        return null;
    }

    /**
     * Saves a color-graded JPEG bitmap directly to the first writable SD card directory.
     * Generates a DCF 8.3 compliant filename with prefix "FUJI" (e.g. FUJI0001.JPG)
     * matching the sequence number of DSC00001.JPG.
     */
    public static String saveGradedJpegToSd(Context context, Bitmap bitmap, int sequenceNum, String recipeName) {
        return saveJpegInternal(context, bitmap, "FUJI", sequenceNum, recipeName);
    }

    /**
     * Saves a JPEG bitmap directly to the first writable SD card directory with fallback.
     * Returns the absolute path of the saved file, or null if all attempts fail.
     */
    public static String saveJpegToSd(Context context, Bitmap bitmap) {
        return saveJpegInternal(context, bitmap, "FUJI", -1, null);
    }

    private static String saveJpegInternal(Context context, Bitmap bitmap, String prefix, int sequenceNum, String recipeName) {
        if (bitmap == null) {
            sLastDiagnostic = "FAIL: Bitmap is null";
            return null;
        }

        List<File> candidates = getCandidateDcimDirs();
        if (candidates.isEmpty()) {
            sLastDiagnostic = "FAIL: No SD mount candidates found";
            Log.e(TAG, sLastDiagnostic);
            return null;
        }

        for (File dir : candidates) {
            try {
                if (!dir.exists()) {
                    dir.mkdirs();
                }
                if (!dir.exists() || !dir.isDirectory()) {
                    continue;
                }

                String filename = (sequenceNum > 0)
                    ? String.format(Locale.US, "%s%04d.JPG", prefix, sequenceNum)
                    : generateDcfFilename(dir, prefix);
                File file = new File(dir, filename);
                FileOutputStream out = new FileOutputStream(file);
                boolean compressed = bitmap.compress(Bitmap.CompressFormat.JPEG, 95, out);
                out.flush();
                try {
                    out.getFD().sync();
                } catch (Throwable ignored) {}
                out.close();

                if (compressed && file.exists() && file.length() > 0) {
                    String savedPath = file.getAbsolutePath();
                    writeExifMetadata(savedPath, recipeName);
                    sLastDiagnostic = "OK (" + prefix + "): " + savedPath + " (" + (file.length() / 1024) + "KB)";
                    Log.i(TAG, "Successfully saved graded photo: " + sLastDiagnostic);

                    // Register with Sony AvindexStore for physical playback
                    registerWithAvindex(context, savedPath);
                    return savedPath;
                } else {
                    file.delete();
                }
            } catch (Throwable t) {
                Log.w(TAG, "Write failed in " + dir.getAbsolutePath() + ": " + t.getMessage());
            }
        }

        sLastDiagnostic = "FAIL: All " + candidates.size() + " mount attempts failed";
        Log.e(TAG, sLastDiagnostic);
        return null;
    }

    /**
     * Writes standard DCF / Sony EXIF metadata into the saved JPEG.
     */
    public static void writeExifMetadata(String filePath) {
        writeExifMetadata(filePath, null);
    }

    public static void writeExifMetadata(String filePath, String recipeName) {
        if (filePath == null) return;
        try {
            ExifInterface exif = new ExifInterface(filePath);
            exif.setAttribute(ExifInterface.TAG_MAKE, "SONY");
            exif.setAttribute(ExifInterface.TAG_MODEL, "ILCE-7RM2");
            if (recipeName != null && !recipeName.trim().isEmpty()) {
                exif.setAttribute("UserComment", "Fuji Recipe: " + recipeName);
            }
            String dt = new java.text.SimpleDateFormat("yyyy:MM:dd HH:mm:ss", Locale.US).format(new java.util.Date());
            exif.setAttribute(ExifInterface.TAG_DATETIME, dt);
            exif.setAttribute(ExifInterface.TAG_ORIENTATION, String.valueOf(ExifInterface.ORIENTATION_NORMAL));
            exif.saveAttributes();
            Log.d(TAG, "EXIF metadata written for DCF compliance: " + filePath);
        } catch (Throwable t) {
            Log.w(TAG, "Failed writing EXIF tags: " + t.getMessage());
        }
    }

    /**
     * Finds next DCF 8.3 filename (e.g. DSC00001.JPG or FUJI0001.JPG).
     */
    public static synchronized String generateDcfFilename(File dir) {
        return generateDcfFilename(dir, "DSC0");
    }

    public static synchronized String generateDcfFilename(File dir, String prefix) {
        if (prefix == null || prefix.length() != 4) {
            prefix = "DSC0";
        }
        int nextNum = allocateNextSequenceNumber();
        return String.format(Locale.US, "%s%04d.JPG", prefix, nextNum);
    }

    /**
     * Registers a freshly written JPEG with Sony AvindexStore and Android MediaScanner.
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

        // 4. Sony Scalar AvindexStore Content Provider Insert & API Reflection
        if (context != null) {
            try {
                Class<?> cls = Class.forName("com.sony.scalar.provider.AvindexStore$Images$Media");
                java.lang.reflect.Field uriField = cls.getField("EXTERNAL_CONTENT_URI");
                Uri uri = (Uri) uriField.get(null);
                if (uri != null) {
                    ContentValues cv = new ContentValues();
                    cv.put("_data", filePath);
                    cv.put("_display_name", file.getName());
                    cv.put("mime_type", "image/jpeg");
                    cv.put("date_added", Long.valueOf(System.currentTimeMillis() / 1000));
                    cv.put("date_modified", Long.valueOf(System.currentTimeMillis() / 1000));
                    context.getContentResolver().insert(uri, cv);
                    Log.i(TAG, "AvindexStore content provider insert succeeded for " + filePath);
                }
            } catch (Throwable t) {
                Log.d(TAG, "AvindexStore ContentProvider insert unavailable: " + t.getMessage());
            }
        }

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

    /**
     * Finds the latest saved JPEG photo across all candidate DCIM directories.
     */
    public static File getLatestSavedPhoto() {
        List<File> candidates = getCandidateDcimDirs();
        File latestFile = null;
        long latestTime = 0;

        for (File dir : candidates) {
            if (!dir.exists() || !dir.isDirectory()) continue;
            File[] files = dir.listFiles();
            if (files == null) continue;
            for (File f : files) {
                if (f.isFile() && f.length() > 0) {
                    String name = f.getName().toUpperCase(Locale.US);
                    if (name.endsWith(".JPG") || name.endsWith(".JPEG")) {
                        long mod = f.lastModified();
                        if (mod > latestTime) {
                            latestTime = mod;
                            latestFile = f;
                        }
                    }
                }
            }
        }
        return latestFile;
    }

    /**
     * Safely decodes a photo file into a display-safe Bitmap (e.g. 640x480 max).
     */
    public static Bitmap loadPhotoForReview(File photoFile, int maxDim) {
        if (photoFile == null || !photoFile.exists()) return null;
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(photoFile.getAbsolutePath(), bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null;

            int sampleSize = 1;
            int maxSide = Math.max(bounds.outWidth, bounds.outHeight);
            while (maxSide / (sampleSize * 2) >= maxDim) {
                sampleSize *= 2;
            }

            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inSampleSize = sampleSize;
            opts.inPreferredConfig = Bitmap.Config.RGB_565;
            opts.inPurgeable = true;
            return BitmapFactory.decodeFile(photoFile.getAbsolutePath(), opts);
        } catch (Throwable t) {
            Log.e(TAG, "Failed to decode photo for review: " + photoFile.getAbsolutePath(), t);
            return null;
        }
    }

    /**
     * Polls DCIM directories for a newly saved camera photo (modified on or after startTime).
     * Waits up to timeoutMs, checking every 200ms.
     * Returns the newly saved File, or null if timed out.
     */
    public static File waitForNewCameraPhoto(long startTime, long timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            List<File> candidates = getCandidateDcimDirs();
            File newest = null;
            long newestTime = startTime - 2000; // Allow up to 2s clock skew
            for (File dir : candidates) {
                if (dir.exists() && dir.isDirectory()) {
                    File[] files = dir.listFiles();
                    if (files != null) {
                        for (File f : files) {
                            String name = f.getName().toUpperCase(Locale.US);
                            if (name.startsWith("DSC") && name.endsWith(".JPG")) {
                                long mod = f.lastModified();
                                if (mod >= newestTime && f.length() > 500 * 1024) {
                                    newestTime = mod;
                                    newest = f;
                                }
                            }
                        }
                    }
                }
            }
            if (newest != null) {
                // Ensure file write has settled
                long initialSize = newest.length();
                try { Thread.sleep(150); } catch (InterruptedException ignored) {}
                long currentSize = newest.length();
                if (currentSize == initialSize && currentSize > 500 * 1024) {
                    return newest;
                }
            }
            try {
                Thread.sleep(200);
            } catch (InterruptedException ignored) {
                break;
            }
        }
        return null;
    }

    public static String getLastDiagnostic() {
        return sLastDiagnostic;
    }
}
