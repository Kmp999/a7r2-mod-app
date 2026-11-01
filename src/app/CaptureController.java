package com.fujilite.a7r2;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapRegionDecoder;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.hardware.Camera;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ShortBuffer;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Manages the capture pipeline, dispatching image processing off the UI thread.
 *
 * Implements:
 *  - Reliable Camera.PictureCallback hardware-triggered capture.
 *  - Instant EVF/LCD preview restoration (< 150ms).
 *  - Native C 17^3 3D LUT + Grain + Clarity grading on 1616x1080 proxy frame.
 *  - Native C Edge-Directed Super-Resolution Upscaling to True 10.6 MP (3984 x 2656 = ~4K).
 *  - Background JPEG encoding to /DCIM/100MSDCF/FUJIxxxx.JPG (Quality = 94, DCF 8.3).
 *  - Registration with Sony AvindexStore for physical camera playback.
 */
public class CaptureController {
    private static final String TAG = "CaptureController";

    public enum CameraState {
        IDLE_PREVIEW,
        HALF_SHUTTER_AF,
        CAPTURING_SINGLE,
        RESTORING_PREVIEW
    }

    private volatile CameraState currentState = CameraState.IDLE_PREVIEW;
    private final AtomicBoolean isCapturing = new AtomicBoolean(false);
    private long lastCaptureTime = 0;
    private static final long MIN_SHOT_INTERVAL_MS = 600;

    // Use native 17x17x17 3D LUT when available, gracefully fallback to Java engine
    private static final boolean USE_NATIVE_ENGINE = true;

    // Heap-safe pixel budget
    private static final int MAX_GRADE_PIXELS = 12 * 1024 * 1024;

    private final Context context;
    private final CameraAdapter cameraAdapter;
    private final UIController uiController;
    private final AppState appState;
    private final ExecutorService processingExecutor;
    private final ExecutorService ioExecutor;
    private final Handler watchdogHandler;
    private final JavaColorEngine javaEngine = new JavaColorEngine();

    public CaptureController(CameraAdapter adapter, final UIController uiController, AppState state) {
        this(null, adapter, uiController, state);
    }

    public CaptureController(Context context, CameraAdapter adapter, final UIController uiController, AppState state) {
        this.context = (context != null) ? context.getApplicationContext() : null;
        this.cameraAdapter = adapter;
        this.uiController = uiController;
        this.appState = state;
        this.watchdogHandler = new Handler(Looper.getMainLooper());
        this.processingExecutor = Executors.newSingleThreadExecutor(new ThreadFactory() {
            @Override
            public Thread newThread(Runnable r) {
                Thread t = new Thread(r, "FujiLite-Grading");
                t.setPriority(Thread.NORM_PRIORITY + 1); // High priority for grading
                return t;
            }
        });
        this.ioExecutor = Executors.newSingleThreadExecutor(new ThreadFactory() {
            @Override
            public Thread newThread(Runnable r) {
                Thread t = new Thread(r, "FujiLite-SD-IO");
                t.setPriority(Thread.NORM_PRIORITY);
                return t;
            }
        });

        // Enhancement 3: Startup sweep for leftover .tmp files from previous hard power-offs
        this.ioExecutor.execute(new Runnable() {
            @Override
            public void run() {
                StorageResolver.cleanupOrphanedTempFiles();
            }
        });

        // Asynchronous Decoupling: Hook into hardware StoreImageCompleteListener if available
        if (cameraAdapter instanceof SonyPmcaAdapter) {
            ((SonyPmcaAdapter) cameraAdapter).setStoreImageListener(new SonyPmcaAdapter.StoreImageListener() {
                @Override
                public void onImageStored(String dir, String filename) {
                    final File rawFile = new File(dir, filename);
                    final int seqNum = StorageResolver.parseSequenceNumber(filename);
                    if (pendingJobsCount.get() >= MAX_PENDING_JOBS) {
                        Log.w(TAG, "Grading queue full on hardware store; skipping background grading.");
                        if (CaptureController.this.uiController != null) {
                            CaptureController.this.uiController.updateStatusAsync("SD BUSY: " + pendingJobsCount.get() + " queued");
                        }
                        return;
                    }
                    pendingJobsCount.incrementAndGet();
                    updateQueueHud();
                    processingExecutor.execute(new Runnable() {
                        @Override
                        public void run() {
                            processStripStreamingCapture(rawFile.getAbsolutePath(), null, seqNum, System.currentTimeMillis());
                        }
                    });
                }
            });
        }
    }

    public static final int MAX_PENDING_JOBS = 8;
    private final java.util.concurrent.atomic.AtomicInteger pendingJobsCount = new java.util.concurrent.atomic.AtomicInteger(0);
    private volatile boolean isShuttingDown = false;

    public int getPendingJobsCount() {
        return pendingJobsCount.get();
    }

    private void updateQueueHud() {
        if (uiController != null) {
            uiController.setPendingGradingCount(pendingJobsCount.get());
        }
    }

    public CameraState getState() {
        return currentState;
    }

    public synchronized void setState(CameraState state) {
        this.currentState = state;
    }

    /** Delivers the graded bitmap to the UI; the UI owns and recycles it. */
    public interface ResultListener {
        void onResult(Bitmap graded, String info);
    }

    private volatile ResultListener resultListener;

    private final Runnable captureWatchdogRunnable = new Runnable() {
        @Override
        public void run() {
            if (isCapturing.compareAndSet(true, false)) {
                Log.w(TAG, "Capture watchdog triggered (>2500ms without completion); restoring preview");
                setState(CameraState.IDLE_PREVIEW);
                appState.setProcessing(false);
                restorePreviewWithWatchdog(3);
                uiController.updateStatusAsync("Ready");
            }
        }
    };

    public void setResultListener(ResultListener l) {
        this.resultListener = l;
    }

    /**
     * Triggers True 10.6 MP Asynchronous Capture:
     *  1. Dispatches Camera.takePicture(null, null, callback).
     *  2. Restores EVF/LCD preview immediately (< 150ms).
     *  3. Dispatches raw DSC0xxxx.JPG writing to SD card on ioExecutor.
     *  4. Dispatches 3D LUT grading + Native Edge-Directed Upscaling to 10.6 MP (3984 x 2656) on processingExecutor.
     */
    public boolean triggerSingleCapture() {
        long now = System.currentTimeMillis();
        if (!isCapturing.compareAndSet(false, true)) {
            Log.w(TAG, "Capture ignored: another shot is currently in progress");
            return false;
        }
        if (now - lastCaptureTime < MIN_SHOT_INTERVAL_MS) {
            isCapturing.set(false);
            Log.d(TAG, "Capture ignored: In cooldown (<" + MIN_SHOT_INTERVAL_MS + "ms)");
            return false;
        }
        lastCaptureTime = now;

        final long captureStartTime = System.currentTimeMillis();

        setState(CameraState.CAPTURING_SINGLE);
        appState.setProcessing(true);
        uiController.updateStatusAsync("Capturing 10.6MP...");

        // Arm failsafe watchdog in case hardware/HAL fails
        watchdogHandler.removeCallbacks(captureWatchdogRunnable);
        watchdogHandler.postDelayed(captureWatchdogRunnable, 3500);

        try {
            if (cameraAdapter instanceof SonyPmcaAdapter) {
                // SONY HARDWARE CAPTURE: Trip BIONZ X with (shutter, null, null) to bypass 1.7MP HAL proxy!
                // BIONZ X writes the full 42.4MP (7952 x 5304) DSC0xxxx.JPG directly to the SD card.
                Camera.ShutterCallback shutterCb = new Camera.ShutterCallback() {
                    @Override
                    public void onShutter() {
                        watchdogHandler.removeCallbacks(captureWatchdogRunnable);
                        try {
                            cameraAdapter.cancelTakePicture();
                            setState(CameraState.RESTORING_PREVIEW);
                            cameraAdapter.resumePreview();
                            restorePreviewWithWatchdog(3);
                        } catch (Throwable t) {
                            Log.e(TAG, "onShutter preview restore error", t);
                        } finally {
                            isCapturing.set(false);
                        }
                    }
                };
                ((SonyPmcaAdapter) cameraAdapter).takeHardwarePicture(shutterCb);

                final int seqNum = StorageResolver.allocateNextSequenceNumber();
                if (pendingJobsCount.get() >= MAX_PENDING_JOBS) {
                    Log.w(TAG, "Grading queue full (" + pendingJobsCount.get() + " >= " + MAX_PENDING_JOBS + "). Saving master DSC0xxxx.JPG only; skipping background strip grading.");
                    if (uiController != null) {
                        uiController.updateStatusAsync("SD BUSY: " + pendingJobsCount.get() + " queued");
                    }
                    return true;
                }
                pendingJobsCount.incrementAndGet();
                updateQueueHud();
                processingExecutor.execute(new Runnable() {
                    @Override
                    public void run() {
                        // Watch for newly saved 42.4MP master DSC0xxxx.JPG on SD card
                        String photoPath = StorageResolver.waitForNewCameraPhoto(4000);
                        if (photoPath != null) {
                            processStripStreamingCapture(photoPath, null, seqNum, captureStartTime);
                        } else {
                            File master = StorageResolver.findRawMasterFile(seqNum);
                            if (master != null) {
                                processStripStreamingCapture(master.getAbsolutePath(), null, seqNum, captureStartTime);
                            }
                        }
                    }
                });
                return true;
            } else {
                cameraAdapter.takePicture(new Camera.PictureCallback() {
                    @Override
                    public void onPictureTaken(final byte[] jpegData, Camera camera) {
                        watchdogHandler.removeCallbacks(captureWatchdogRunnable);
                        try {
                            // 1. Immediately cancel take picture and restore Live View EVF/LCD (< 20ms)
                            cameraAdapter.cancelTakePicture();
                            setState(CameraState.RESTORING_PREVIEW);
                            cameraAdapter.resumePreview();
                            restorePreviewWithWatchdog(3);

                            if (jpegData == null || jpegData.length == 0) {
                                uiController.updateStatusAsync("Capture failed: Empty buffer");
                                return;
                            }

                            // 2. Synchronize DCF Sequence Number
                            final int seqNum = StorageResolver.allocateNextSequenceNumber();

                            // Enhancement 2: Check queue bounding (MAX_PENDING_JOBS = 8)
                            if (pendingJobsCount.get() >= MAX_PENDING_JOBS) {
                                Log.w(TAG, "Grading queue full (" + pendingJobsCount.get() + " >= " + MAX_PENDING_JOBS + "). Saving master DSC0xxxx.JPG only; skipping background strip grading.");
                                if (uiController != null) {
                                    uiController.updateStatusAsync("SD BUSY: " + pendingJobsCount.get() + " queued");
                                }
                                ioExecutor.execute(new Runnable() {
                                    @Override
                                    public void run() {
                                        StorageResolver.saveRawJpegToSd(context, jpegData, seqNum);
                                    }
                                });
                                return;
                            }

                            // 3. Immediately hand off jpegData to background processingExecutor and return (< 30ms UI return)
                            pendingJobsCount.incrementAndGet();
                            updateQueueHud();
                            processingExecutor.execute(new Runnable() {
                                @Override
                                public void run() {
                                    processStripStreamingCapture(null, jpegData, seqNum, captureStartTime);
                                }
                            });

                        } catch (Throwable t) {
                            Log.e(TAG, "onPictureTaken error", t);
                            if (uiController != null) {
                                uiController.updateStatusAsync("ERR: " + t.getClass().getSimpleName() + ": " + t.getMessage());
                            }
                        } finally {
                            isCapturing.set(false);
                        }
                    }
                });
                return true;
            }
        } catch (Throwable t) {
            watchdogHandler.removeCallbacks(captureWatchdogRunnable);
            Log.e(TAG, "takePicture failed", t);
            restorePreviewWithWatchdog(3);
            isCapturing.set(false);
            setState(CameraState.IDLE_PREVIEW);
            appState.setProcessing(false);
            if (uiController != null) {
                uiController.updateStatusAsync("ERR: " + t.getClass().getSimpleName() + ": " + t.getMessage());
            }
            return false;
        }
    }

    public void triggerCapture() {
        triggerSingleCapture();
    }

    public void onShutterKeyUp() {
        if (cameraAdapter instanceof SonyPmcaAdapter) {
            ((SonyPmcaAdapter) cameraAdapter).cancelTakePicture();
        }
    }

    /**
     * Retries preview restoration up to 3 times (20ms apart) to prevent black EVF screens.
     */
    private void restorePreviewWithWatchdog(final int retriesLeft) {
        if (retriesLeft <= 0) {
            setState(CameraState.IDLE_PREVIEW);
            return;
        }

        try {
            cameraAdapter.resumePreview();
            setState(CameraState.IDLE_PREVIEW);
            Log.d(TAG, "EVF preview restored successfully");
        } catch (Throwable t) {
            Log.w(TAG, "Preview restore retry scheduled (" + retriesLeft + " left)");
            watchdogHandler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    restorePreviewWithWatchdog(retriesLeft - 1);
                }
            }, 20);
        }
    }

    /**
     * Native 42.4MP-to-10.6MP Downsampled Strip-Streaming Processing Engine:
     * - Native 42.4MP Hardware Master: Full 7952x5304 capture (DSC0xxxx.JPG) is kept 100% intact (NEVER deleted).
     * - Downsamples 42.4MP -> 10.6MP (3976x2652) using native 1/2 DCT downsampling (inSampleSize = 2).
     * - Strict Dalvik Heap Safety: Decodes 128 source rows at a time, producing 3976x64 strips (~508 KB RAM).
     * - Seamless Strip Boundaries: deterministic row-seeded PRNG and continuous grain/dither offsets.
     * - Safe Temp-File-Then-Rename: writes to fuji_xxxx.jpg.tmp then renames to fuji_xxxx.jpg.
     * - EXIF Preservation: copies camera hardware EXIF tags from DSC0xxxx.JPG, updating dims to 3976x2652.
     */
    public void processStripStreamingCapture(String rawJpegPath, final byte[] inMemoryJpegData, final int seqNum, final long captureStartTime) {
        appState.setProcessing(true);
        try {
            // 1. Resolve master raw JPEG file (Zero Data Destruction: NEVER delete DSC0xxxx.JPG)
            if (rawJpegPath == null && inMemoryJpegData != null && inMemoryJpegData.length > 0) {
                rawJpegPath = StorageResolver.saveRawJpegToSd(context, inMemoryJpegData, seqNum);
            }
            if (rawJpegPath == null) {
                File found = StorageResolver.findRawMasterFile(seqNum);
                if (found != null) {
                    rawJpegPath = found.getAbsolutePath();
                }
            }
            if (rawJpegPath == null || !new File(rawJpegPath).exists()) {
                Log.e(TAG, "Master capture file not found for seq " + seqNum);
                if (uiController != null) uiController.updateStatusAsync("ERR: Raw file not found");
                return;
            }

            File rawMasterFile = new File(rawJpegPath);
            // Verify file size stability and JPEG EOI marker (0xFF 0xD9)
            StorageResolver.waitForJpegComplete(rawMasterFile, 3000);

            final FujiRecipe recipe = appState.getActiveRecipe();
            final String presetName = appState.getCurrentPresetName();

            // 2. Initialize BitmapRegionDecoder for 42.4MP -> 10.6MP downsampled decoding
            BitmapRegionDecoder decoder = null;
            try {
                decoder = BitmapRegionDecoder.newInstance(rawJpegPath, false);
            } catch (Throwable t) {
                Log.w(TAG, "BitmapRegionDecoder initialization failed for " + rawJpegPath + ": " + t.getMessage());
            }

            // Fallback for mock test environments or non-region images
            if (decoder == null) {
                Bitmap fallbackBmp = BitmapFactory.decodeFile(rawJpegPath);
                if (fallbackBmp == null && inMemoryJpegData != null) {
                    fallbackBmp = BitmapFactory.decodeByteArray(inMemoryJpegData, 0, inMemoryJpegData.length);
                }
                if (fallbackBmp != null) {
                    int w = fallbackBmp.getWidth();
                    int h = fallbackBmp.getHeight();
                    ColorEngine.applyLutAndGrainBitmapSafe(fallbackBmp, recipe.grainEffect.ordinal(), recipe.clarity);
                    String saved = StorageResolver.saveGradedJpegToSd(context, fallbackBmp, seqNum, presetName);
                    long ms = System.currentTimeMillis() - captureStartTime;
                    deliverResult(fallbackBmp, presetName, w, h, ms, saved);
                    return;
                }
                Log.e(TAG, "Failed to decode raw master: " + rawJpegPath);
                return;
            }

            final int srcW = decoder.getWidth();
            final int srcH = decoder.getHeight();

            // Step 1: Mode Selection (FAST_10MP vs MASTER_42MP)
            final AppState.OutputResolutionMode resMode = (appState != null)
                ? appState.getOutputResolutionMode()
                : AppState.OutputResolutionMode.FAST_10MP;

            int targetSample = (srcW > 4000) ? 2 : 1;
            int srcStripH = 128;
            if (resMode == AppState.OutputResolutionMode.MASTER_42MP) {
                targetSample = 1;
                srcStripH = 64;
            }
            final int dstW = (targetSample == 1) ? srcW : (srcW > 4000 ? srcW / 2 : srcW); // dstW = (srcW > 4000) ? srcW / 2 : srcW
            final int dstH = (targetSample == 1) ? srcH : ((srcH > 3000) ? srcH / 2 : srcH);
            Log.i(TAG, "Processing Strip-Streaming [" + resMode.label + "]: " + srcW + "x" + srcH + " -> " + dstW + "x" + dstH + " (" + rawJpegPath + ")");

            // 3. Prepare target and temporary files (Safe temp-file-then-rename)
            final File targetOutputFile = StorageResolver.getGradedOutputFile(seqNum);
            final File tmpFile = new File(targetOutputFile.getParentFile(), targetOutputFile.getName() + ".tmp");
            if (tmpFile.exists()) {
                tmpFile.delete();
            }

            // 4. Precompute 17^3 3D LUT once per image in native memory
            if (USE_NATIVE_ENGINE && ColorEngine.isNativeAvailable()) {
                byte[] baseLut = (context != null && recipe.filmSim.lutFile != null)
                    ? LutManager.getLutTable(context, recipe.filmSim.lutFile) : null;
                if (baseLut != null) {
                    ColorEngine.precomputeLut17WithBaseSafe(
                        baseLut,
                        recipe.computeCcmQ8(),
                        recipe.generateToneCurve('R'),
                        recipe.generateToneCurve('G'),
                        recipe.generateToneCurve('B')
                    );
                } else {
                    ColorEngine.precomputeLut17Safe(
                        recipe.computeCcmQ8(),
                        recipe.generateToneCurve('R'),
                        recipe.generateToneCurve('G'),
                        recipe.generateToneCurve('B')
                    );
                }
            }

            final int grainStrength = recipe.grainEffect.ordinal();
            final int clarityStrength = recipe.clarity;

            // 5. Check Pure Native C Zero-JNI-Copy Fast-Path
            boolean streamSuccess = false;
            if (ColorEngine.isNativeAvailable() && ColorEngine.nativeProcessImageStreamFastSafe(rawJpegPath, tmpFile.getAbsolutePath(), grainStrength, clarityStrength)) {
                streamSuccess = true;
                if (decoder != null) {
                    decoder.recycle();
                    decoder = null;
                }
            } else {
                // Fallback: Java BitmapRegionDecoder strip-streaming loop
                long streamHandle = ColorEngine.openJpegStreamSafe(tmpFile.getAbsolutePath(), dstW, dstH, 95);
                if (streamHandle == 0) {
                    Log.w(TAG, "Native stream unavailable, falling back to direct bitmap grading and saving modified image");
                    Bitmap fallbackBmp = BitmapFactory.decodeFile(rawJpegPath);
                    if (fallbackBmp == null && inMemoryJpegData != null) {
                        fallbackBmp = BitmapFactory.decodeByteArray(inMemoryJpegData, 0, inMemoryJpegData.length);
                    }
                    if (fallbackBmp != null) {
                        ColorEngine.applyLutAndGrainBitmapSafe(fallbackBmp, grainStrength, clarityStrength);
                        String saved = StorageResolver.saveGradedJpegToSd(context, fallbackBmp, seqNum, presetName);
                        long ms = System.currentTimeMillis() - captureStartTime;
                        deliverResult(fallbackBmp, presetName, fallbackBmp.getWidth(), fallbackBmp.getHeight(), ms, saved);
                    }
                    if (decoder != null) decoder.recycle();
                    return;
                }

                // 6. Strict Dalvik Heap Management: Process in source strips (producing 64 output rows per strip)
                final int numStrips = (srcH + srcStripH - 1) / srcStripH;

                // Allocate direct buffer capped at 2.03 MB max (7952 * 64 * 4 for 42.4MP, 3976 * 64 * 4 for 10.6MP)
                java.nio.ByteBuffer stripDirect = java.nio.ByteBuffer.allocateDirect(dstW * 64 * 4);

                BitmapFactory.Options opts = new BitmapFactory.Options();
                opts.inSampleSize = (srcW > 4000) ? 2 : 1;
                if (resMode == AppState.OutputResolutionMode.MASTER_42MP) {
                    opts.inSampleSize = 1;
                }
                opts.inPreferredConfig = Bitmap.Config.ARGB_8888; // Enhancement 1: Upgrade to ARGB_8888 precision
                opts.inDither = false;
                opts.inMutable = true;

                streamSuccess = true;
                int outputRowOffset = 0;
                try {
                    for (int stripIdx = 0; stripIdx < numStrips; stripIdx++) {
                        // Enhancement 3: Check abort flag on power-off / shutdown
                        if (isShuttingDown) {
                            Log.w(TAG, "Aborting strip streaming: camera shutdown in progress");
                            streamSuccess = false;
                            break;
                        }

                        int srcY = stripIdx * srcStripH;
                        int currentSrcH = Math.min(srcStripH, srcH - srcY);
                        Rect rect = new Rect(0, srcY, srcW, srcY + currentSrcH);

                        Bitmap stripBmp = null;
                        try {
                            try {
                                stripBmp = decoder.decodeRegion(rect, opts);
                            } catch (OutOfMemoryError oom) {
                                Log.w(TAG, "OOM decoding strip in ARGB_8888, falling back to RGB_565", oom);
                                opts.inPreferredConfig = Bitmap.Config.RGB_565;
                                stripBmp = decoder.decodeRegion(rect, opts);
                            }
                            if (stripBmp == null) {
                                Log.e(TAG, "decodeRegion returned null for strip at source row " + srcY);
                                streamSuccess = false;
                                break;
                            }
                            int outStripW = stripBmp.getWidth();  // 3976 or 7952
                            int outStripH = stripBmp.getHeight(); // 64 (or 28 / 56 for final partial strip)

                            stripDirect.clear();
                            stripBmp.copyPixelsToBuffer(stripDirect);
                            stripDirect.rewind();

                            // Native C fused grading (deterministic row-seeded PRNG) + JPEG stream write
                            boolean stripOk = ColorEngine.gradeAndWriteStreamStripSafe(
                                streamHandle, stripDirect, outStripW, outStripH, outputRowOffset, grainStrength, clarityStrength
                            );
                            if (!stripOk) {
                                Log.e(TAG, "Failed encoding strip at output row " + outputRowOffset);
                                streamSuccess = false;
                                break;
                            }
                            outputRowOffset += outStripH;
                        } finally {
                            // Crucial: immediately recycle strip Bitmap before decoding next row block
                            if (stripBmp != null && !stripBmp.isRecycled()) {
                                stripBmp.recycle();
                                stripBmp = null;
                            }
                        }
                    }
                } finally {
                    // Deterministic stream closure in finally block
                    boolean closed = ColorEngine.closeJpegStreamSafe(streamHandle, streamSuccess);
                    streamHandle = 0;
                    if (!closed) streamSuccess = false;
                    if (decoder != null) {
                        decoder.recycle();
                        decoder = null;
                    }
                    if (!streamSuccess && tmpFile.exists()) {
                        tmpFile.delete(); // Delete incomplete .tmp file on failure/abort
                    }
                }
            }

            // 7. Atomic temp-file-then-rename to fuji_xxxx.jpg
            String gradedSavedPath = null;
            if (streamSuccess && tmpFile.exists() && tmpFile.length() > 0) {
                if (targetOutputFile.exists()) {
                    targetOutputFile.delete();
                }
                boolean renamed = tmpFile.renameTo(targetOutputFile);
                if (!renamed) {
                    StorageResolver.copyFile(tmpFile, targetOutputFile);
                    tmpFile.delete();
                }
                gradedSavedPath = targetOutputFile.getAbsolutePath();

                // 8. Copy original camera EXIF metadata from DSC0xxxx.JPG to fuji_xxxx.jpg with dynamic dims & DR mode stamp
                StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName, recipe.dynamicRange.label, dstW, dstH);
                StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName, dstW, dstH);

                // 9. Register with Sony AvindexStore
                StorageResolver.registerWithAvindex(context, gradedSavedPath);
            } else {
                if (tmpFile.exists()) {
                    tmpFile.delete();
                }
                Log.e(TAG, "Strip streaming failed to produce valid output");
            }

            final long totalMs = System.currentTimeMillis() - captureStartTime;
            final float mp = (float)(dstW * dstH) / 1000000.0f;
            final String savedFileName = (gradedSavedPath != null) ? new File(gradedSavedPath).getName() : String.format(Locale.US, "fuji_%04d.jpg", seqNum);
            final String telemetry = String.format(Locale.US, "SAVED %s (%dx%d | %.1fMP) in %dms",
                savedFileName, dstW, dstH, mp, totalMs);
            uiController.updateStatusAsync(telemetry);
            Log.i(TAG, telemetry + " -> " + gradedSavedPath);

            // 10. Load display-safe review bitmap (<= 960x640) for UI display
            Bitmap reviewBmp = null;
            if (targetOutputFile.exists()) {
                reviewBmp = StorageResolver.loadPhotoForReview(targetOutputFile, 960);
            }
            if (reviewBmp != null) {
                if (uiController != null) uiController.showQuickReview(reviewBmp);
                deliverResult(reviewBmp, presetName, dstW, dstH, totalMs, gradedSavedPath);
            }

        } catch (Throwable t) {
            Log.e(TAG, "Error in processStripStreamingCapture", t);
            if (uiController != null) {
                uiController.updateStatusAsync("ERR: " + t.getClass().getSimpleName() + ": " + t.getMessage());
            }
        } finally {
            appState.setProcessing(false);
            int remaining = pendingJobsCount.decrementAndGet();
            if (remaining < 0) pendingJobsCount.set(0);
            updateQueueHud();
            System.gc();
        }
    }

    public void processAndUpscaleCapture(final byte[] jpegData, final int seqNum, final long captureStartTime) {
        processStripStreamingCapture(null, jpegData, seqNum, captureStartTime);
    }

    // Preserved for backward compatibility with unit tests
    public void saveFullResJpegDirectly(byte[] rawJpeg, int seqNum) {
        String saved = StorageResolver.saveRawJpegToSd(context, rawJpeg, seqNum);
        if (saved != null) {
            uiController.updateStatusAsync("SAVED 42.4MP: " + saved);
            Log.i(TAG, "SAVED 42.4MP: " + saved);
        }
    }

    public void processQuickReviewProxy(byte[] jpegData, int seqNum) {
        processAndUpscaleCapture(jpegData, seqNum, System.currentTimeMillis());
    }

    private void deliverResult(Bitmap bitmap, String preset, int w, int h, long ms, String savedPath) {
        float mp = (float)(w * h) / 1000000.0f;
        String baseInfo = String.format(Locale.US, "%s  %.1fMP (GRADED)  %d ms", preset, mp, ms);
        if (savedPath != null) {
            baseInfo += "\nSAVED: " + savedPath;
        }
        ResultListener l = resultListener;
        if (l != null) {
            // Downsample review bitmap (<= 960x640) for UI display to prevent OpenGL texture crash (> 2048x2048)
            Bitmap displayBmp = null;
            if (bitmap != null && !bitmap.isRecycled() && (bitmap.getWidth() > 1920 || bitmap.getHeight() > 1080)) {
                try {
                    int targetW = 960;
                    int targetH = (h * targetW) / w;
                    displayBmp = Bitmap.createScaledBitmap(bitmap, targetW, targetH, true);
                } catch (Throwable t) {
                    Log.w(TAG, "Failed creating review display bitmap", t);
                }
            }
            if (displayBmp == null) {
                displayBmp = bitmap;
            }
            l.onResult(displayBmp, baseInfo);
            if (displayBmp != bitmap && bitmap != null && !bitmap.isRecycled()) {
                bitmap.recycle();
            }
        } else if (bitmap != null && !bitmap.isRecycled()) {
            bitmap.recycle();
        }
    }

    public void runDemo() {
        if (appState.isProcessing()) {
            return;
        }
        appState.setProcessing(true);
        uiController.updateStatusAsync("Rendering demo...");

        processingExecutor.execute(new Runnable() {
            @Override
            public void run() {
                final long start = System.currentTimeMillis();
                try {
                    int w = 640;
                    int h = 480;
                    Bitmap bitmap = makeTestImage(w, h);

                    short[] pixels = new short[w * h];
                    ShortBuffer buffer = ShortBuffer.wrap(pixels);
                    bitmap.copyPixelsToBuffer(buffer);

                    boolean graded = false;
                    if (USE_NATIVE_ENGINE && ColorEngine.isNativeAvailable()) {
                        FujiRecipe recipe = appState.getActiveRecipe();
                        ColorEngine.precomputeLut17Safe(
                            recipe.computeCcmQ8(),
                            recipe.generateToneCurve('R'),
                            recipe.generateToneCurve('G'),
                            recipe.generateToneCurve('B')
                        );
                        graded = ColorEngine.applyLut17RGB565Safe(pixels, w * h);
                    }
                    if (!graded) {
                        javaEngine.init(appState.getCurrentPresetId());
                        javaEngine.processRGB565(pixels, w, h);
                    }

                    buffer.rewind();
                    bitmap.copyPixelsFromBuffer(buffer);

                    String savedPath = StorageResolver.saveJpegToSd(context, bitmap);
                    long duration = System.currentTimeMillis() - start;
                    appState.setLastProcessingTimeMs(duration);
                    deliverResult(bitmap, appState.getCurrentPresetName() + " (demo)", w, h, duration, savedPath);
                } catch (Throwable t) {
                    Log.e(TAG, "Demo failed", t);
                    uiController.updateStatusAsync("Demo error: " + t.getClass().getSimpleName());
                } finally {
                    appState.setProcessing(false);
                }
            }
        });
    }

    private Bitmap makeTestImage(int w, int h) {
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.RGB_565);
        Canvas c = new Canvas(bmp);
        Paint p = new Paint();
        int[] bars = {
            Color.rgb(255, 255, 255), Color.rgb(255, 0, 0), Color.rgb(0, 255, 0),
            Color.rgb(0, 0, 255), Color.rgb(255, 255, 0), Color.rgb(0, 255, 255),
            Color.rgb(255, 0, 255), Color.rgb(128, 128, 128)
        };
        int bw = w / bars.length;
        for (int i = 0; i < bars.length; i++) {
            p.setColor(bars[i]);
            c.drawRect(i * bw, 0, (i + 1) * bw, h / 2, p);
        }
        for (int x = 0; x < w; x++) {
            int v = (int) (255L * x / w);
            p.setColor(Color.rgb(v, v, v));
            c.drawRect(x, h / 2, x + 1, h, p);
        }
        return bmp;
    }

    public void shutdown() {
        isShuttingDown = true;
        watchdogHandler.removeCallbacks(captureWatchdogRunnable);
        processingExecutor.shutdown();
        ioExecutor.shutdown();
        try {
            if (!processingExecutor.awaitTermination(2500, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                Log.w(TAG, "processingExecutor did not terminate within 2500ms; forcing shutdown");
                processingExecutor.shutdownNow();
            }
        } catch (InterruptedException e) {
            processingExecutor.shutdownNow();
        }
        try {
            if (!ioExecutor.awaitTermination(1000, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                ioExecutor.shutdownNow();
            }
        } catch (InterruptedException e) {
            ioExecutor.shutdownNow();
        }
    }
}
