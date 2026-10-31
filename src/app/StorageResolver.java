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
    private static volatile boolean sSequenceInitialized = false;
    private static volatile File sCachedWorkingDir = null;

    private StorageResolver() {}

    /**
     * Resolves candidate DCIM directories in priority order.
     */
    public static List<File> getCandidateDcimDirs() {
        List<File> list = new ArrayList<File>();
        if (sCachedWorkingDir != null && sCachedWorkingDir.exists()) {
            list.add(sCachedWorkingDir);
        }
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
        if (sSequenceInitialized) {
            int num = sFileSequence++;
            if (sFileSequence > 9999) sFileSequence = 1;
            return num;
        }

        List<File> candidates = getCandidateDcimDirs();
        int maxSeq = 0;
        for (File dir : candidates) {
            if (dir.exists() && dir.isDirectory()) {
                File[] files = dir.listFiles();
                if (files != null) {
                    for (File f : files) {
                        String name = f.getName().toUpperCase(Locale.US);
                        if (name.endsWith(".JPG") || name.endsWith(".JPEG")) {
                            if (name.startsWith("FUJI_") && name.length() >= 9) {
                                try {
                                    int dot = name.indexOf('.');
                                    String numStr = name.substring(5, dot > 5 ? dot : 9);
                                    int val = Integer.parseInt(numStr);
                                    if (val > maxSeq) maxSeq = val;
                                } catch (Throwable ignored) {}
                            } else if (name.length() == 12) {
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
        }
        int nextNum = (maxSeq > 0) ? (maxSeq + 1) : sFileSequence;
        if (nextNum > 9999) nextNum = 1;
        sFileSequence = nextNum + 1;
        sSequenceInitialized = true;
        return nextNum;
    }

    /**
     * Extracts the 4-digit sequence number from a DCF filename (e.g. DSC00123.JPG -> 123, fuji_0123.jpg -> 123).
     */
    public static int parseSequenceNumber(String filename) {
        if (filename == null) return -1;
        String name = new File(filename).getName().toUpperCase(Locale.US);
        if (name.startsWith("FUJI_") && (name.endsWith(".JPG") || name.endsWith(".JPEG"))) {
            try {
                int dot = name.indexOf('.');
                String numStr = name.substring(5, dot > 5 ? dot : 9);
                return Integer.parseInt(numStr);
            } catch (Throwable ignored) {}
        }
        if (name.endsWith(".JPG") && name.length() == 12) {
            try {
                return Integer.parseInt(name.substring(4, 8));
            } catch (Throwable ignored) {}
        }
        return -1;
    }

    /**
     * Finds the latest DCF hardware photo filename currently residing on the SD card.
     */
    public static String getLatestDcfFilename() {
        List<File> candidates = getCandidateDcimDirs();
        File newest = null;
        long newestTime = 0;
        for (File dir : candidates) {
            if (dir.exists() && dir.isDirectory()) {
                File[] files = dir.listFiles();
                if (files != null) {
                    for (File f : files) {
                        String name = f.getName().toUpperCase(Locale.US);
                        if (name.startsWith("DSC0") && name.endsWith(".JPG")) {
                            long t = f.lastModified();
                            if (t > newestTime) {
                                newestTime = t;
                                newest = f;
                            }
                        }
                    }
                }
            }
        }
        return newest != null ? newest.getName() : null;
    }

    /**
     * Fast fallback directory poller (Requirement 1.3).
     * Polls DCIM candidate folders every 50ms up to timeoutMs for a newly written DSC0xxxx.JPG by BIONZ X.
     */
    public static File pollForNewHardwareJpeg(long preCaptureTime, String lastKnownFilename, long timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        List<File> candidates = getCandidateDcimDirs();
        long threshold = preCaptureTime - 1500; // Allow slight timestamp skew on FAT32

        while (System.currentTimeMillis() < deadline) {
            for (File dir : candidates) {
                if (!dir.exists() || !dir.isDirectory()) continue;
                File[] files = dir.listFiles();
                if (files == null) continue;

                File candidate = null;
                long newestTime = 0;

                for (File f : files) {
                    String name = f.getName().toUpperCase(Locale.US);
                    if (name.startsWith("DSC0") && name.endsWith(".JPG")) {
                        if (lastKnownFilename != null && name.equalsIgnoreCase(lastKnownFilename)) {
                            continue;
                        }
                        long mtime = f.lastModified();
                        if (mtime >= threshold && mtime >= newestTime && f.length() > 10240) {
                            newestTime = mtime;
                            candidate = f;
                        }
                    }
                }

                if (candidate != null) {
                    if (isJpegFileComplete(candidate)) {
                        Log.i(TAG, "Hardware JPEG verified complete via DCIM poller: " + candidate.getAbsolutePath() + " (" + (candidate.length() / 1024) + "KB)");
                        return candidate;
                    }
                }
            }
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                break;
            }
        }
        Log.w(TAG, "Hardware JPEG poller timed out after " + timeoutMs + "ms");
        return null;
    }

    /**
     * Checks if a JPEG file has been completely written and flushed to disk by BIONZ X.
     * Prevents fatal partial-decode SIGSEGV and EBUSY file-locking collisions on FAT32.
     */
    public static boolean isJpegFileComplete(File file) {
        if (file == null || !file.exists()) return false;
        long len = file.length();
        if (len < 50000) return false;

        java.io.RandomAccessFile raf = null;
        try {
            raf = new java.io.RandomAccessFile(file, "r");
            long fileLen = raf.length();
            if (fileLen < 50000) return false;
            raf.seek(fileLen - 2);
            int b1 = raf.read();
            int b2 = raf.read();
            return (b1 == 0xFF && b2 == 0xD9); // JPEG End-Of-Image marker
        } catch (Throwable t) {
            return false;
        } finally {
            if (raf != null) {
                try { raf.close(); } catch (Throwable ignored) {}
            }
        }
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

    public static final String PREFIX_FUJI = "FUJI"; // Supported for DCF compatibility

    /**
     * Resolves the target file path on SD card using the mandatory fuji_%04d.jpg naming format (Requirement 2).
     */
    public static File getGradedOutputFile(int sequenceNum) {
        List<File> candidates = getCandidateDcimDirs();
        for (File dir : candidates) {
            try {
                if (!dir.exists()) dir.mkdirs();
                if (dir.exists() && dir.isDirectory()) {
                    String filename = String.format(Locale.US, "fuji_%04d.jpg", sequenceNum);
                    return new File(dir, filename);
                }
            } catch (Throwable ignored) {}
        }
        return new File("/mnt/sdcard/DCIM/100MSDCF", String.format(Locale.US, "fuji_%04d.jpg", sequenceNum));
    }

    /**
     * Saves a color-graded JPEG bitmap directly to the first writable SD card directory.
     * Generates a filename starting with "fuji_" (e.g. fuji_0001.jpg).
     */
    public static String saveGradedJpegToSd(Context context, Bitmap bitmap, int sequenceNum, String recipeName) {
        return saveJpegInternal(context, bitmap, "fuji_", sequenceNum, recipeName);
    }

    /**
     * Saves a JPEG bitmap directly to the first writable SD card directory with fallback.
     * Returns the absolute path of the saved file, or null if all attempts fail.
     */
    public static String saveJpegToSd(Context context, Bitmap bitmap) {
        return saveJpegInternal(context, bitmap, "fuji_", -1, null);
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

                String filename;
                if (sequenceNum > 0) {
                    if ("fuji_".equalsIgnoreCase(prefix) || "FUJI".equalsIgnoreCase(prefix)) {
                        filename = String.format(Locale.US, "fuji_%04d.jpg", sequenceNum);
                    } else {
                        filename = String.format(Locale.US, "%s%04d.JPG", prefix, sequenceNum);
                    }
                } else {
                    filename = generateDcfFilename(dir, prefix);
                }
                File file = new File(dir, filename);
                File tmpFile = new File(dir, filename + ".tmp");
                java.io.BufferedOutputStream out = new java.io.BufferedOutputStream(new FileOutputStream(tmpFile), 65536);
                boolean compressed = bitmap.compress(Bitmap.CompressFormat.JPEG, 94, out);
                out.flush();
                out.close();

                if (compressed && tmpFile.exists() && tmpFile.length() > 0) {
                    if (file.exists()) {
                        file.delete();
                    }
                    boolean renamed = tmpFile.renameTo(file);
                    if (!renamed) {
                        copyFile(tmpFile, file);
                        tmpFile.delete();
                    }
                    String savedPath = file.getAbsolutePath();
                    File master = findRawMasterFile(sequenceNum);
                    if (master != null && master.exists()) {
                        copyExifMetadata(master.getAbsolutePath(), savedPath, recipeName);
                    } else {
                        writeExifMetadata(savedPath, recipeName);
                    }
                    sLastDiagnostic = "OK (" + prefix + "): " + savedPath + " (" + (file.length() / 1024) + "KB)";
                    Log.i(TAG, "Successfully saved graded photo: " + sLastDiagnostic);

                    // Register with Sony AvindexStore for physical playback
                    registerWithAvindex(context, savedPath);
                    return savedPath;
                } else {
                    if (tmpFile.exists()) {
                        tmpFile.delete();
                    }
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
     * Finds the original master capture file (DSC0xxxx.JPG) for a given sequence number.
     * Note: Original master files are strictly preserved and NEVER deleted.
     */
    public static File findRawMasterFile(int sequenceNum) {
        if (sequenceNum <= 0) return null;
        List<File> candidates = getCandidateDcimDirs();
        for (File dir : candidates) {
            if (!dir.exists() || !dir.isDirectory()) continue;
            File rawDsc = new File(dir, String.format(Locale.US, "DSC0%04d.JPG", sequenceNum));
            if (rawDsc.exists() && rawDsc.length() > 0) {
                return rawDsc;
            }
        }
        return null;
    }

    /**
     * Copies file content from src to dst.
     */
    public static void copyFile(File src, File dst) throws java.io.IOException {
        java.io.FileInputStream in = new java.io.FileInputStream(src);
        try {
            java.io.FileOutputStream out = new java.io.FileOutputStream(dst);
            try {
                byte[] buf = new byte[65536];
                int len;
                while ((len = in.read(buf)) > 0) {
                    out.write(buf, 0, len);
                }
                out.flush();
            } finally {
                out.close();
            }
        } finally {
            in.close();
        }
    }

    /**
     * Registers a freshly saved native graded JPEG, preserves original EXIF metadata, and registers with Avindex.
     */
    public static void postProcessGradedFile(Context context, String filePath, int sequenceNum, String recipeName) {
        if (filePath == null) return;
        File master = findRawMasterFile(sequenceNum);
        if (master != null && master.exists()) {
            copyExifMetadata(master.getAbsolutePath(), filePath, recipeName);
        } else {
            writeExifMetadata(filePath, recipeName);
        }
        registerWithAvindex(context, filePath);
    }

    /**
     * Copies all camera EXIF metadata from source master capture (DSC0xxxx.JPG) to graded JPEG (fuji_xxxx.jpg).
     * Preserves hardware ISP tags (aperture, shutter speed, ISO, focal length, date, white balance, etc.).
     */
    public static void copyExifMetadata(String srcPath, String dstPath, String recipeName) {
        if (dstPath == null) return;
        if (srcPath == null || !new File(srcPath).exists()) {
            writeExifMetadata(dstPath, recipeName);
            return;
        }
        try {
            ExifInterface srcExif = new ExifInterface(srcPath);
            ExifInterface dstExif = new ExifInterface(dstPath);

            String[] tags = new String[] {
                ExifInterface.TAG_DATETIME,
                ExifInterface.TAG_FLASH,
                ExifInterface.TAG_FOCAL_LENGTH,
                ExifInterface.TAG_GPS_ALTITUDE,
                ExifInterface.TAG_GPS_ALTITUDE_REF,
                ExifInterface.TAG_GPS_DATESTAMP,
                ExifInterface.TAG_GPS_LATITUDE,
                ExifInterface.TAG_GPS_LATITUDE_REF,
                ExifInterface.TAG_GPS_LONGITUDE,
                ExifInterface.TAG_GPS_LONGITUDE_REF,
                ExifInterface.TAG_GPS_PROCESSING_METHOD,
                ExifInterface.TAG_GPS_TIMESTAMP,
                ExifInterface.TAG_IMAGE_LENGTH,
                ExifInterface.TAG_IMAGE_WIDTH,
                ExifInterface.TAG_ISO,
                ExifInterface.TAG_MAKE,
                ExifInterface.TAG_MODEL,
                ExifInterface.TAG_ORIENTATION,
                ExifInterface.TAG_WHITE_BALANCE,
                "ExposureTime",
                "FNumber",
                "ISOSpeedRatings",
                "DateTimeOriginal",
                "DateTimeDigitized",
                "SubSecTime",
                "SubSecTimeOriginal",
                "SubSecTimeDigitized",
                "ExposureProgram",
                "MeteringMode",
                "LightSource",
                "ExposureBiasValue",
                "MaxApertureValue",
                "DigitalZoomRatio",
                "FocalLengthIn35mmFilm"
            };

            for (String tag : tags) {
                try {
                    String val = srcExif.getAttribute(tag);
                    if (val != null) {
                        dstExif.setAttribute(tag, val);
                    }
                } catch (Throwable ignored) {}
            }

            dstExif.setAttribute(ExifInterface.TAG_MAKE, "SONY");
            dstExif.setAttribute(ExifInterface.TAG_MODEL, "ILCE-7RM2");
            if (recipeName != null && !recipeName.trim().isEmpty()) {
                dstExif.setAttribute("UserComment", "Fuji Recipe: " + recipeName);
            }
            dstExif.saveAttributes();
            Log.d(TAG, "EXIF metadata successfully preserved from " + srcPath + " to " + dstPath);
        } catch (Throwable t) {
            Log.w(TAG, "Failed preserving EXIF metadata: " + t.getMessage());
            writeExifMetadata(dstPath, recipeName);
        }
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
     * Waits up to timeoutMs, checking every 150-200ms.
     * Returns the newly saved File, or null if timed out.
     */
    public static File waitForNewCameraPhoto(long startTime, long timeoutMs) {
        return waitForNewCameraPhoto(startTime, null, timeoutMs);
    }

    public static File waitForNewCameraPhoto(long startTime, File preCaptureLatest, long timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            List<File> candidates = getCandidateDcimDirs();
            File newest = null;
            long newestTime = startTime - 3000; // Allow up to 3s clock skew
            for (File dir : candidates) {
                if (dir.exists() && dir.isDirectory()) {
                    File[] files = dir.listFiles();
                    if (files != null) {
                        for (File f : files) {
                            String name = f.getName().toUpperCase(Locale.US);
                            if (name.startsWith("DSC") && name.endsWith(".JPG")) {
                                if (preCaptureLatest != null && f.getAbsolutePath().equals(preCaptureLatest.getAbsolutePath()) && f.length() == preCaptureLatest.length()) {
                                    continue;
                                }
                                long mod = f.lastModified();
                                if (mod >= newestTime && f.length() > 200 * 1024) {
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
                try { Thread.sleep(200); } catch (InterruptedException ignored) {}
                long currentSize = newest.length();
                if (currentSize == initialSize && currentSize > 200 * 1024) {
                    return newest;
                }
            }
            try {
                Thread.sleep(150);
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
