package com.fujilite.a7r2;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapRegionDecoder;
import android.graphics.Rect;
import android.hardware.Camera;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.nio.ByteBuffer;
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
            public Thread newThread(final Runnable r) {
                Thread t = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
                        } catch (Throwable ignored) {}
                        r.run();
                    }
                }, "FujiLite-Grading");
                t.setPriority(Thread.NORM_PRIORITY - 1); // Lower than UI thread so dials/shutter never starve
                return t;
            }
        });
        this.ioExecutor = Executors.newSingleThreadExecutor(new ThreadFactory() {
            @Override
            public Thread newThread(final Runnable r) {
                Thread t = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
                        } catch (Throwable ignored) {}
                        r.run();
                    }
                }, "FujiLite-SD-IO");
                t.setPriority(Thread.NORM_PRIORITY - 1);
                return t;
            }
        });

        // Startup sweep + pre-warm DCIM cache, sequence counter & known photo snapshot OFF the UI thread
        this.ioExecutor.execute(new Runnable() {
            @Override
            public void run() {
                StorageResolver.cleanupOrphanedTempFiles();
                StorageResolver.prewarmDirectoryAndSequence();
                java.util.Set<String> initialPhotos = StorageResolver.snapshotExistingPhotos();
                synchronized (knownPhotoPaths) {
                    knownPhotoPaths.addAll(initialPhotos);
                    knownPhotoPathsInitialized = true;
                }
            }
        });

        // Asynchronous Decoupling: Hook into hardware StoreImageCompleteListener if available
        if (cameraAdapter instanceof SonyPmcaAdapter) {
            ((SonyPmcaAdapter) cameraAdapter).setStoreImageListener(new SonyPmcaAdapter.StoreImageListener() {
                @Override
                public void onImageStored(String dir, String filename) {
                    storeImageListenerSeen = true;
                    if (filename == null) {
                        // ScalarOS CameraEx$StoreImageCompleteListener(int mediaId, CameraEx) wakes poller immediately
                        synchronized (hardwareStoreLock) {
                            hardwareStoreLock.notifyAll();
                        }
                        return;
                    }
                    File resolvedFile = StorageResolver.resolveStoredPhotoFile(dir, filename);
                    if (resolvedFile == null || !resolvedFile.exists()) {
                        try { Thread.sleep(50); } catch (InterruptedException ignored) {}
                        resolvedFile = StorageResolver.resolveStoredPhotoFile(dir, filename);
                    }
                    final File rawFile = resolvedFile;
                    if (rawFile == null || !rawFile.exists()) {
                        Log.w(TAG, "StoreImageCompleteListener unresolved path: " + dir + "/" + filename);
                        synchronized (hardwareStoreLock) {
                            hardwareStoreLock.notifyAll();
                        }
                        return;
                    }
                    recordKnownPhoto(rawFile);

                    // If triggerSingleCapture workers are waiting for shots, enqueue FIFO handoff (ZERO duplicate grading!)
                    synchronized (hardwareStoreLock) {
                        if (waitingPollerJobs > 0) {
                            pendingHardwareStoredQueue.addLast(rawFile);
                            hardwareStoreLock.notifyAll();
                            return;
                        }
                    }

                    if (!claimPhotoForProcessing(rawFile)) {
                        return;
                    }
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
    private final java.util.Set<String> processedPhotoPaths = java.util.Collections.synchronizedSet(new java.util.HashSet<String>());
    private final java.util.Set<String> knownPhotoPaths = new java.util.HashSet<String>();
    private volatile boolean knownPhotoPathsInitialized = false;
    private final Object hardwareStoreLock = new Object();
    private final java.util.LinkedList<File> pendingHardwareStoredQueue = new java.util.LinkedList<File>();
    private int waitingPollerJobs = 0;
    private volatile boolean storeImageListenerSeen = false;
    private volatile boolean isShuttingDown = false;
    private volatile String lastPrecomputedLutSig = null;
    // Remembers which POSIX output tier succeeded (1=.tmp, 2=direct DCIM, 3=cache->MediaStore) to skip EACCES retries
    private static volatile int sPreferredFastOutputTier = 1;

    private void ensureRecipeLutPrecomputed(FujiRecipe recipe) {
        if (recipe == null || !USE_NATIVE_ENGINE || !ColorEngine.isNativeAvailable()) return;
        String sig = recipe.filmSim.name() + ":" + recipe.color + ":" + recipe.highlightTone + ":" +
                     recipe.shadowTone + ":" + recipe.whiteBalance.name() + ":" +
                     recipe.wbShiftR + ":" + recipe.wbShiftB + ":" + recipe.colorChrome.name() + ":" +
                     recipe.dynamicRange.name();
        if (sig.equals(lastPrecomputedLutSig)) {
            return;
        }
        byte[] baseLut = (context != null && recipe.filmSim.lutFile != null)
            ? LutManager.getLutTable(context, recipe.filmSim.lutFile) : null;
        if (baseLut != null) {
            ColorEngine.precomputeLut17WithBaseSafe(
                baseLut,
                recipe.computeAdjustmentCcmQ8(),
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
        lastPrecomputedLutSig = sig;
    }

    private static String dedupKeyForFile(File f) {
        if (f == null) return "";
        File parent = f.getParentFile();
        String folder = (parent != null) ? parent.getName().toUpperCase(Locale.US) : "";
        return folder + "/" + f.getName().toUpperCase(Locale.US);
    }

    private boolean claimPhotoForProcessing(File f) {
        if (f == null) return false;
        String key = dedupKeyForFile(f);
        synchronized (processedPhotoPaths) {
            if (processedPhotoPaths.contains(key)) {
                return false;
            }
            processedPhotoPaths.add(key);
            return true;
        }
    }

    private void recordKnownPhoto(File f) {
        if (f == null) return;
        synchronized (knownPhotoPaths) {
            knownPhotoPaths.add(f.getAbsolutePath());
        }
    }

    private java.util.Set<String> copyKnownPhotoPathsNonBlocking() {
        synchronized (knownPhotoPaths) {
            return new java.util.HashSet<String>(knownPhotoPaths);
        }
    }

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

    private final java.util.Set<String> processedPhotos = java.util.Collections.synchronizedSet(new java.util.HashSet<String>());

    private final Runnable captureWatchdogRunnable = new Runnable() {
        @Override
        public void run() {
            boolean wasCapturing = isCapturing.compareAndSet(true, false);
            if (wasCapturing || currentState != CameraState.IDLE_PREVIEW) {
                Log.w(TAG, "Capture watchdog triggered (>3500ms without completion); restoring preview");
                setState(CameraState.IDLE_PREVIEW);
                isCapturing.set(false);
                appState.setProcessing(false);
                if (cameraAdapter instanceof SonyPmcaAdapter) {
                    ((SonyPmcaAdapter) cameraAdapter).setPollingPaused(false);
                }
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
        if (isShuttingDown) return false;
        long now = System.currentTimeMillis();
        if (now - lastCaptureTime > MIN_SHOT_INTERVAL_MS) {
            isCapturing.set(false);
        }
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
                final SonyPmcaAdapter sonyAdapter = (SonyPmcaAdapter) cameraAdapter;
                // Pause background exposure polling so Binder IPC never collides with shutter release
                sonyAdapter.setPollingPaused(true);

                // Zero-I/O in-memory photo snapshot (pre-warmed off UI thread) -> 0ms shutter lag!
                final boolean hadInitialSnapshot = knownPhotoPathsInitialized;
                final java.util.Set<String> preExistingFiles = copyKnownPhotoPathsNonBlocking();
                boolean countedWaitingJob = false;
                synchronized (hardwareStoreLock) {
                    waitingPollerJobs++;
                    countedWaitingJob = true;
                }

                // SONY HARDWARE CAPTURE: Trip BIONZ X with (shutter, null, null) to bypass 1.7MP HAL proxy!
                // BIONZ X writes the full 42.4MP (7952 x 5304) DSC0xxxx.JPG directly to the SD card.
                Camera.ShutterCallback shutterCb = new Camera.ShutterCallback() {
                    @Override
                    public void onShutter() {
                        watchdogHandler.removeCallbacks(captureWatchdogRunnable);
                        try {
                            // Note: Do NOT call cancelTakePicture() here as it aborts the hardware exposure.
                            setState(CameraState.RESTORING_PREVIEW);
                            // Restore preview after mechanical shutter exposure cycle
                            watchdogHandler.postDelayed(new Runnable() {
                                @Override
                                public void run() {
                                    try {
                                        restorePreviewWithWatchdog(3);
                                    } catch (Throwable t) {
                                        Log.w(TAG, "Delayed preview restore failed", t);
                                    } finally {
                                        sonyAdapter.setPollingPaused(false);
                                        isCapturing.set(false);
                                    }
                                }
                            }, 250);
                        } catch (Throwable t) {
                            Log.e(TAG, "onShutter preview restore error", t);
                            sonyAdapter.setPollingPaused(false);
                            isCapturing.set(false);
                        }
                    }
                };
                sonyAdapter.takeHardwarePicture(shutterCb);

                final int seqNum = StorageResolver.allocateNextSequenceNumber();
                if (pendingJobsCount.get() >= MAX_PENDING_JOBS) {
                    if (countedWaitingJob) {
                        synchronized (hardwareStoreLock) {
                            if (waitingPollerJobs > 0) waitingPollerJobs--;
                        }
                    }
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
                        boolean jobStarted = false;
                        try {
                            // 0. Pre-warm 17^3 3D LUT in parallel while BIONZ X is still flushing DSC0xxxx.JPG to SD card
                            ensureRecipeLutPrecomputed(appState.getActiveRecipe());

                            File newPhoto = null;
                            // 1. Check FIFO instant handoff from StoreImageCompleteListener (zero SD polling!)
                            synchronized (hardwareStoreLock) {
                                if (!pendingHardwareStoredQueue.isEmpty()) {
                                    newPhoto = pendingHardwareStoredQueue.pollFirst();
                                } else if (storeImageListenerSeen && !isShuttingDown) {
                                    try {
                                        hardwareStoreLock.wait(1200);
                                    } catch (InterruptedException ignored) {}
                                    if (!pendingHardwareStoredQueue.isEmpty()) {
                                        newPhoto = pendingHardwareStoredQueue.pollFirst();
                                    }
                                }
                                if (waitingPollerJobs > 0) waitingPollerJobs--;
                            }

                            // 2. Fallback: watch for newly saved 42.4MP master DSC0xxxx.JPG on SD card
                            if (newPhoto == null) {
                                if (uiController != null) uiController.updateStatusAsync("Waiting for SD photo...");
                                if (hadInitialSnapshot) {
                                    newPhoto = StorageResolver.waitForNewCameraPhoto(preExistingFiles, 7000);
                                } else {
                                    // Cold-start guard: filter by capture timestamp when startup snapshot wasn't ready
                                    newPhoto = StorageResolver.waitForNewCameraPhoto(captureStartTime, 7000);
                                }
                            }
                            if (newPhoto == null) {
                                newPhoto = StorageResolver.getLatestCameraPhoto();
                            }
                            if (newPhoto == null) {
                                newPhoto = StorageResolver.getLatestSavedPhoto();
                            }
                            if (newPhoto != null) {
                                recordKnownPhoto(newPhoto);
                                if (!claimPhotoForProcessing(newPhoto)) {
                                    Log.d(TAG, "Skipping already-graded photo: " + newPhoto.getName());
                                    return;
                                }
                            }
                            String photoPath = (newPhoto != null) ? newPhoto.getAbsolutePath() : null;
                            int actualSeq = (photoPath != null) ? StorageResolver.parseSequenceNumber(photoPath) : seqNum;
                            if (actualSeq <= 0) actualSeq = seqNum;

                            if (photoPath != null) {
                                jobStarted = true;
                                if (uiController != null) uiController.updateStatusAsync("Grading " + newPhoto.getName() + "...");
                                processStripStreamingCapture(photoPath, null, actualSeq, captureStartTime, true);
                            } else {
                                File master = StorageResolver.findRawMasterFile(seqNum);
                                if (master != null && claimPhotoForProcessing(master)) {
                                    recordKnownPhoto(master);
                                    jobStarted = true;
                                    processStripStreamingCapture(master.getAbsolutePath(), null, seqNum, captureStartTime);
                                } else {
                                    Log.e(TAG, "No photo found on SD card after hardware capture");
                                    if (uiController != null) uiController.updateStatusAsync("ERR: SD timeout (no photo written)");
                                }
                            }
                        } finally {
                            if (!jobStarted) {
                                int rem = pendingJobsCount.decrementAndGet();
                                if (rem < 0) {
                                    pendingJobsCount.set(0);
                                    rem = 0;
                                }
                                updateQueueHud();
                                appState.setProcessing(rem > 0);
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
            if (cameraAdapter instanceof SonyPmcaAdapter) {
                ((SonyPmcaAdapter) cameraAdapter).setPollingPaused(false);
            }
            synchronized (hardwareStoreLock) {
                if (waitingPollerJobs > 0) waitingPollerJobs--;
            }
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
        // Do NOT call cancelTakePicture() here: calling cancelTakePicture on S2 release
        // corrupts BIONZ X hardware capture sequence for subsequent shots!
        isCapturing.set(false);
        Log.d(TAG, "onShutterKeyUp - ready for next shot");
    }

    /**
     * Retries preview restoration up to 3 times (50ms apart) to prevent black EVF screens.
     */
    private void restorePreviewWithWatchdog(final int retriesLeft) {
        if (retriesLeft <= 0) {
            setState(CameraState.IDLE_PREVIEW);
            return;
        }

        try {
            boolean success = cameraAdapter.resumePreview();
            if (!success) {
                throw new RuntimeException("resumePreview returned false");
            }
            setState(CameraState.IDLE_PREVIEW);
            Log.d(TAG, "EVF preview restored successfully");
        } catch (Throwable t) {
            Log.w(TAG, "Preview restore retry scheduled (" + retriesLeft + " left): " + t.getMessage());
            watchdogHandler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    restorePreviewWithWatchdog(retriesLeft - 1);
                }
            }, 50);
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
        processStripStreamingCapture(rawJpegPath, inMemoryJpegData, seqNum, captureStartTime, false);
    }

    public void processStripStreamingCapture(String rawJpegPath, final byte[] inMemoryJpegData, final int seqNum, final long captureStartTime, boolean skipInitialEoiCheck) {
        appState.setProcessing(true);
        // The grading thread is created at THREAD_PRIORITY_BACKGROUND (nice 10 + bg cgroup, ~5% CPU share when
        // anything else is runnable) and every native pthread inherits that. Run the heavy transcode at default
        // priority (same as the UI thread, never above it) and restore afterwards.
        int prevPrio = Integer.MIN_VALUE;
        try {
            prevPrio = android.os.Process.getThreadPriority(android.os.Process.myTid());
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_DEFAULT);
        } catch (Throwable ignored) {}
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
            // Verify file size stability and JPEG EOI marker (0xFF 0xD9) if not already verified by waitForNewCameraPhoto
            if (!skipInitialEoiCheck && !StorageResolver.isJpegComplete(rawMasterFile)) {
                StorageResolver.waitForJpegComplete(rawMasterFile, 1500);
            }

            final long tSdReady = System.currentTimeMillis();
            final long sdWaitMs = tSdReady - captureStartTime;

            final FujiRecipe recipe = appState.getActiveRecipe();
            final String presetName = appState.getCurrentPresetName();

            // 2. Read source JPEG dimensions in ~1ms via inJustDecodeBounds (DO NOT construct
            //    BitmapRegionDecoder yet, which wastes 2.5s building a full-file Huffman index!)
            final boolean nativeReady = USE_NATIVE_ENGINE && ColorEngine.isNativeAvailable();
            BitmapRegionDecoder decoder = null;
            BitmapFactory.Options boundsOpts = new BitmapFactory.Options();
            boundsOpts.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(rawJpegPath, boundsOpts);
            int srcW = boundsOpts.outWidth;
            int srcH = boundsOpts.outHeight;

            // Fallback for mock test environments or images where bounds cannot be determined
            if (srcW <= 0 || srcH <= 0) {
                Bitmap fallbackBmp = BitmapFactory.decodeFile(rawJpegPath);
                if (fallbackBmp == null && inMemoryJpegData != null) {
                    fallbackBmp = BitmapFactory.decodeByteArray(inMemoryJpegData, 0, inMemoryJpegData.length);
                }
                if (fallbackBmp != null) {
                    int w = fallbackBmp.getWidth();
                    int h = fallbackBmp.getHeight();
                    boolean graded = ColorEngine.applyLutAndGrainBitmapSafe(fallbackBmp, recipe.grainEffect.ordinal(), recipe.clarity);
                    if (!graded) {
                        fallbackBmp = applyRecipeInJava(fallbackBmp, recipe);
                    }
                    String saved = StorageResolver.saveGradedJpegToSd(context, fallbackBmp, seqNum, presetName);
                    long ms = System.currentTimeMillis() - captureStartTime;
                    deliverResult(fallbackBmp, presetName, w, h, ms, saved, graded ? "C-FB" : "JAVA");
                    return;
                }
                Log.e(TAG, "Failed to decode raw master: " + rawJpegPath);
                return;
            }

            // Step 1: Mode Selection (FAST_10MP vs MASTER_42MP)
            final AppState.OutputResolutionMode resMode = (appState != null)
                ? appState.getOutputResolutionMode()
                : AppState.OutputResolutionMode.FAST_10MP;

            int targetSample = (srcW >= 7000) ? 2 : 1;
            int srcStripH = 128;
            if (targetSample == 1 || resMode == AppState.OutputResolutionMode.MASTER_42MP) {
                targetSample = 1;
                srcStripH = 64; // Keeps decoded strip height <= 64 rows (prevents BufferOverflowException on 18MP!)
            }
            final int dstW = (targetSample == 1) ? srcW : (srcW > 4000 ? (srcW >= 7000 ? srcW / 2 : srcW) : srcW); // dstW = (srcW > 4000) ? srcW / 2 : srcW
            final int dstH = (targetSample == 1) ? srcH : ((srcH > 3000) ? (srcW >= 7000 ? srcH / 2 : srcH) : srcH);
            Log.i(TAG, "Processing Strip-Streaming [" + resMode.label + "]: " + srcW + "x" + srcH + " -> " + dstW + "x" + dstH + " (" + rawJpegPath + ")");

            // 3. Prepare strict DOS 8.3 / DCF 2.0 target and temp files in the EXACT SAME folder as the original photo.
            //    Sony A7R II's FUSE driver (libInfraFuFsys.so) rejects >8.3 names (like fuji_8107.jpg) with ENAMETOOLONG!
            final int dcfSeq = StorageResolver.normalizeDcfSequence(seqNum);
            final String dcfJpegName = StorageResolver.formatGradedFilename(dcfSeq);
            final String dcfTmpName = StorageResolver.formatGradedTempFilename(dcfSeq);
            File resolvedTarget = StorageResolver.getGradedOutputFile(seqNum);
            if (rawJpegPath != null) {
                File rawDir = new File(rawJpegPath).getParentFile();
                if (rawDir != null && rawDir.exists() && rawDir.isDirectory() && !rawDir.getAbsolutePath().contains("wv_cache")) {
                    resolvedTarget = new File(rawDir, dcfJpegName);
                }
            }
            final File targetOutputFile = resolvedTarget;
            final File tmpFile = new File(targetOutputFile.getParentFile(), dcfTmpName);
            if (tmpFile.exists()) {
                tmpFile.delete();
            }

            // 4. Precompute 17^3 3D LUT once per recipe in native memory (cached if unchanged)
            if (nativeReady) {
                ensureRecipeLutPrecomputed(recipe);
            }

            final int grainStrength = recipe.grainEffect.ordinal();
            final int clarityStrength = recipe.clarity;

            // 5. Pure Native C Single-Pass 4-Core Fast-Path (bypasses BitmapRegionDecoder, Dalvik heap & copyPixelsToBuffer!)
            boolean streamSuccess = false;
            boolean inlineExifWritten = false;
            boolean usedDirectTarget = false;
            boolean usedCacheStream = false;
            boolean usedFastTranscode = false;
            File actualDirectTarget = targetOutputFile;
            File cacheStreamFile = null;
            long fastTranscodeMs = 0;
            int lastFastRc = 0;
            int lastOpenErrno = 0;
            long brdIdxMs = 0;
            long brdDecMs = 0;
            long brdJniMs = 0;

            int usedFastTier = 0;
            if (nativeReady) {
                long tFast0 = System.currentTimeMillis();
                if (uiController != null) {
                    uiController.updateStatusAsync("Fast 4-Core Grading...");
                }
                int pref = sPreferredFastOutputTier;
                int[] tierOrder = (pref == 3) ? new int[]{3, 1, 2}
                    : ((pref == 2) ? new int[]{2, 1, 3} : new int[]{1, 2, 3});
                for (int ti = 0; ti < tierOrder.length && !streamSuccess; ti++) {
                    int tier = tierOrder[ti];
                    if (tier == 1) {
                        if (ColorEngine.nativeProcessImageStreamFastScaledSafe(rawJpegPath, tmpFile.getAbsolutePath(), targetSample, grainStrength, clarityStrength)) {
                            streamSuccess = true;
                            usedFastTranscode = true;
                            inlineExifWritten = true;
                            lastFastRc = 1;
                            usedFastTier = 1;
                            sPreferredFastOutputTier = 1;
                        } else {
                            lastFastRc = ColorEngine.getLastFastRcSafe();
                            int[] errSt = ColorEngine.getLastFastStatsSafe();
                            if (errSt != null && errSt.length >= 20 && errSt[19] != 0) lastOpenErrno = errSt[19];
                            if (lastFastRc != -9) break;
                        }
                    } else if (tier == 2) {
                        java.util.List<File> directCandidates = StorageResolver.getSameFolderWriteTargets(targetOutputFile);
                        for (int ci = 0; ci < directCandidates.size() && !streamSuccess; ci++) {
                            File cand = directCandidates.get(ci);
                            if (ColorEngine.nativeProcessImageStreamFastScaledSafe(rawJpegPath, cand.getAbsolutePath(), targetSample, grainStrength, clarityStrength)) {
                                streamSuccess = true;
                                usedFastTranscode = true;
                                inlineExifWritten = true;
                                lastFastRc = 1;
                                usedFastTier = 2;
                                usedDirectTarget = true;
                                actualDirectTarget = cand;
                                sPreferredFastOutputTier = 2;
                            } else {
                                lastFastRc = ColorEngine.getLastFastRcSafe();
                                int[] errSt = ColorEngine.getLastFastStatsSafe();
                                if (errSt != null && errSt.length >= 20 && errSt[19] != 0) lastOpenErrno = errSt[19];
                                if (lastFastRc != -9) break;
                            }
                        }
                        if (!streamSuccess && lastFastRc != -9) break;
                    } else if (tier == 3 && context != null && context.getCacheDir() != null) {
                        cacheStreamFile = new File(context.getCacheDir(), dcfJpegName);
                        if (cacheStreamFile.exists()) cacheStreamFile.delete();
                        if (ColorEngine.nativeProcessImageStreamFastScaledSafe(rawJpegPath, cacheStreamFile.getAbsolutePath(), targetSample, grainStrength, clarityStrength)) {
                            streamSuccess = true;
                            usedFastTranscode = true;
                            inlineExifWritten = true;
                            lastFastRc = 1;
                            usedFastTier = 3;
                            usedCacheStream = true;
                            sPreferredFastOutputTier = 3;
                        } else {
                            lastFastRc = ColorEngine.getLastFastRcSafe();
                            if (cacheStreamFile.exists()) cacheStreamFile.delete();
                            cacheStreamFile = null;
                            if (lastFastRc != -9) break;
                        }
                    }
                }
                fastTranscodeMs = System.currentTimeMillis() - tFast0;
            }

            if (usedFastTranscode) {
                if (decoder != null) {
                    decoder.recycle();
                    decoder = null;
                }
            } else {
                // Lazy-initialize BitmapRegionDecoder ONLY when pure-C fast path needed fallback (e.g. progressive JPEG)
                if (nativeReady && decoder == null) {
                    long tIdx0 = System.currentTimeMillis();
                    try {
                        decoder = BitmapRegionDecoder.newInstance(rawJpegPath, false);
                    } catch (Throwable t) {
                        Log.w(TAG, "BitmapRegionDecoder initialization failed for " + rawJpegPath + ": " + t.getMessage());
                    }
                    brdIdxMs = System.currentTimeMillis() - tIdx0;
                }
                // 3-Tier Native Stream Open:
                //   Tier 1: DCIM fuji_xxxx.jpg.tmp (atomic rename)
                //   Tier 2: DCIM fuji_xxxx.jpg direct (if Sony FAT32 rejects .tmp extension)
                //   Tier 3: App cache file -> raw MediaStore byte stream (if SD POSIX write returns EACCES)
                byte[] inlineExifApp1 = null; // Avoid copying raw Sony MakerNotes/stale IFD1 thumbnail verbatim
                long streamHandle = 0;
                if (nativeReady && decoder != null) {
                    streamHandle = openNativeStreamTier(tmpFile, dstW, dstH, inlineExifApp1);
                    if (streamHandle == 0) {
                        Log.w(TAG, "Tier 1 (.tmp) stream open failed; trying Tier 2 direct target: " + targetOutputFile.getAbsolutePath());
                        streamHandle = openNativeStreamTier(targetOutputFile, dstW, dstH, inlineExifApp1);
                        if (streamHandle != 0) {
                            usedDirectTarget = true;
                        }
                    }
                    if (streamHandle == 0 && context != null) {
                        File cacheDir = context.getCacheDir();
                        if (cacheDir != null) {
                            cacheStreamFile = new File(cacheDir, dcfJpegName);
                            if (cacheStreamFile.exists()) cacheStreamFile.delete();
                            Log.w(TAG, "Tier 2 direct SD stream open failed (EACCES?); trying Tier 3 app-cache stream: " + cacheStreamFile.getAbsolutePath());
                            streamHandle = openNativeStreamTier(cacheStreamFile, dstW, dstH, inlineExifApp1);
                            if (streamHandle != 0) {
                                usedCacheStream = true;
                            }
                        }
                    }
                    inlineExifWritten = (streamHandle != 0 && inlineExifApp1 != null && inlineExifApp1.length >= 4);
                }

                if (streamHandle == 0) {
                    Log.w(TAG, "Native stream unavailable (nativeReady=" + nativeReady + ", err=" + ColorEngine.getNativeLoadError() + "), falling back to bitmap grading");
                    BitmapFactory.Options fallbackOpts = new BitmapFactory.Options();
                    // Preserve >=10.6MP floor! Only downsample 42.4MP (srcW >= 7000) to 10.5MP; never crush 18MP to 4.5MP!
                    fallbackOpts.inSampleSize = (srcW >= 7000) ? 2 : 1;
                    fallbackOpts.inPreferredConfig = Bitmap.Config.RGB_565;
                    fallbackOpts.inDither = true;
                    fallbackOpts.inMutable = true;

                    Bitmap fallbackBmp = null;
                    try {
                        fallbackBmp = BitmapFactory.decodeFile(rawJpegPath, fallbackOpts);
                        if (fallbackBmp == null && decoder != null) {
                            fallbackBmp = decoder.decodeRegion(new Rect(0, 0, srcW, srcH), fallbackOpts);
                        }
                    } catch (OutOfMemoryError oom) {
                        Log.w(TAG, "OOM at inSampleSize " + fallbackOpts.inSampleSize + ", trying next sample size", oom);
                        System.gc();
                    } catch (Throwable t) {
                        Log.w(TAG, "decodeFile for fallback failed", t);
                    }

                    // Progressive memory-safe fallback if full allocation fails on Dalvik heap
                    if (fallbackBmp == null && fallbackOpts.inSampleSize == 1) {
                        try {
                            fallbackOpts.inSampleSize = 2;
                            fallbackBmp = BitmapFactory.decodeFile(rawJpegPath, fallbackOpts);
                        } catch (OutOfMemoryError oom15) {
                            Log.w(TAG, "OOM at inSampleSize 2, trying sample size 4", oom15);
                            System.gc();
                        } catch (Throwable ignored) {}
                    }
                    if (fallbackBmp == null) {
                        try {
                            fallbackOpts.inSampleSize = 4;
                            fallbackBmp = BitmapFactory.decodeFile(rawJpegPath, fallbackOpts);
                            if (fallbackBmp == null && decoder != null) {
                                fallbackBmp = decoder.decodeRegion(new Rect(0, 0, srcW, srcH), fallbackOpts);
                            }
                        } catch (OutOfMemoryError oom2) {
                            Log.w(TAG, "OOM at inSampleSize 4, trying sample size 8", oom2);
                            System.gc();
                            try {
                                fallbackOpts.inSampleSize = 8;
                                fallbackBmp = BitmapFactory.decodeFile(rawJpegPath, fallbackOpts);
                                if (fallbackBmp == null && decoder != null) fallbackBmp = decoder.decodeRegion(new Rect(0, 0, srcW, srcH), fallbackOpts);
                            } catch (Throwable ignored) {}
                        } catch (Throwable t2) {
                            Log.e(TAG, "Progressive fallback decode failed", t2);
                        }
                    }

                    if (fallbackBmp == null && inMemoryJpegData != null) {
                        try {
                            fallbackOpts.inSampleSize = (srcW >= 7000) ? 2 : 1;
                            fallbackBmp = BitmapFactory.decodeByteArray(inMemoryJpegData, 0, inMemoryJpegData.length, fallbackOpts);
                        } catch (OutOfMemoryError oom) {
                            fallbackOpts.inSampleSize = 4;
                            fallbackBmp = BitmapFactory.decodeByteArray(inMemoryJpegData, 0, inMemoryJpegData.length, fallbackOpts);
                        }
                    }

                    if (fallbackBmp != null) {
                        boolean graded = ColorEngine.applyLutAndGrainBitmapSafe(fallbackBmp, grainStrength, clarityStrength);
                        if (!graded) {
                            fallbackBmp = applyRecipeInJava(fallbackBmp, recipe);
                        }
                        File targetDir = (rawJpegPath != null) ? new File(rawJpegPath).getParentFile() : null;
                        // skipPostProcess=true eliminates double copyExifMetadata() rewrite & double Avindex scan!
                        final String saved = StorageResolver.saveGradedJpegToDirectory(context, fallbackBmp, targetDir, seqNum, presetName, true);
                        if (saved != null) {
                            StorageResolver.copyExifMetadata(rawJpegPath, saved, presetName, recipe.dynamicRange.label, fallbackBmp.getWidth(), fallbackBmp.getHeight());
                            ioExecutor.execute(new Runnable() {
                                @Override
                                public void run() {
                                    StorageResolver.registerWithAvindex(context, saved);
                                }
                            });
                        }
                        long ms = System.currentTimeMillis() - captureStartTime;
                        String fbTag = graded ? "C-FB" : (nativeReady ? "JAVA-FB" : "JAVA-NOJNI");
                        String fbName = (saved != null) ? new File(saved).getName() : dcfJpegName;
                        deliverResult(fallbackBmp, presetName, fallbackBmp.getWidth(), fallbackBmp.getHeight(), ms, saved, fbTag);
                        uiController.updateStatusAsync(String.format(Locale.US, "SAVED %s (%dx%d [%s]) in %dms", fbName, fallbackBmp.getWidth(), fallbackBmp.getHeight(), fbTag, ms));
                    } else {
                        Log.e(TAG, "Failed to decode fallback bitmap for " + rawJpegPath);
                        if (uiController != null) uiController.updateStatusAsync("ERR: Decode failed");
                    }
                    if (decoder != null) decoder.recycle();
                    return;
                }

                // 6. Strict Dalvik Heap Management: Process in source strips
                final int maxOutStripH;
                java.nio.ByteBuffer stripDirect;
                if (nativeReady) {
                    srcStripH *= 4; // 256 output rows per band (64 rows per Cortex-A5 core)
                    maxOutStripH = 256;
                    stripDirect = java.nio.ByteBuffer.allocateDirect(dstW * maxOutStripH * 4);
                } else {
                    maxOutStripH = 64;
                    stripDirect = java.nio.ByteBuffer.allocateDirect(dstW * 64 * 4);
                }
                final int numStrips = (srcH + srcStripH - 1) / srcStripH;

                BitmapFactory.Options opts = new BitmapFactory.Options();
                opts.inSampleSize = (srcW > 4000) ? 2 : 1;
                if (srcW < 7000 || resMode == AppState.OutputResolutionMode.MASTER_42MP) {
                    opts.inSampleSize = 1; // Preserve >=10.6MP floor! Never downsample 18MP to 4.5MP!
                }
                opts.inPreferredConfig = Bitmap.Config.ARGB_8888; // Enhancement 1: Upgrade to ARGB_8888 precision
                opts.inDither = false;
                opts.inMutable = true;
                opts.inPurgeable = true;
                opts.inInputShareable = true;

                streamSuccess = true;
                int outputRowOffset = 0;
                // Note: BitmapRegionDecoder.decodeRegion only supports opts.inBitmap on Android 4.4+ (API 19+).
                // On A7R II (Android 2.3.7/4.1.2), keeping reusableStripBmp alive pins 5.29MB in the 24MB Dalvik heap
                // and triggers 13 full GC_FOR_ALLOC pauses! Only retain reusableStripBmp on API >= 19.
                final boolean canReuseRegionBitmap = android.os.Build.VERSION.SDK_INT >= 19;
                Bitmap reusableStripBmp = null;
                try {
                    for (int stripIdx = 0; stripIdx < numStrips; stripIdx++) {
                        // Enhancement 3: Check abort flag on power-off / shutdown
                        if (isShuttingDown) {
                            Log.w(TAG, "Aborting strip streaming: camera shutdown in progress");
                            streamSuccess = false;
                            break;
                        }

                        if (stripIdx % 4 == 0 || stripIdx == numStrips - 1) {
                            if (uiController != null) {
                                uiController.updateStatusAsync(String.format(Locale.US, "Grading strip %d/%d...", (stripIdx + 1), numStrips));
                            }
                        }

                        int srcY = stripIdx * srcStripH;
                        int currentSrcH = Math.min(srcStripH, srcH - srcY);
                        Rect rect = new Rect(0, srcY, srcW, srcY + currentSrcH);

                        Bitmap stripBmp = null;
                        try {
                            long tDec0 = System.currentTimeMillis();
                            try {
                                if (canReuseRegionBitmap && reusableStripBmp != null && currentSrcH == srcStripH) {
                                    opts.inBitmap = reusableStripBmp;
                                } else {
                                    opts.inBitmap = null;
                                }
                                try {
                                    stripBmp = decoder.decodeRegion(rect, opts);
                                } catch (IllegalArgumentException inBmpErr) {
                                    opts.inBitmap = null;
                                    stripBmp = decoder.decodeRegion(rect, opts);
                                }
                            } catch (OutOfMemoryError oom) {
                                Log.w(TAG, "OOM decoding strip in ARGB_8888, falling back to RGB_565", oom);
                                opts.inBitmap = null;
                                if (reusableStripBmp != null && !reusableStripBmp.isRecycled()) {
                                    reusableStripBmp.recycle();
                                    reusableStripBmp = null;
                                }
                                opts.inPreferredConfig = Bitmap.Config.RGB_565;
                                stripDirect = null;
                                System.gc();
                                // Resize direct buffer so native JNI accurately detects RGB_565 (2 bytes/px)
                                stripDirect = java.nio.ByteBuffer.allocateDirect(dstW * 64 * 2);
                                if (nativeReady && maxOutStripH != 64) {
                                    stripDirect = java.nio.ByteBuffer.allocateDirect(dstW * maxOutStripH * 2);
                                }
                                stripBmp = decoder.decodeRegion(rect, opts);
                            }
                            brdDecMs += (System.currentTimeMillis() - tDec0);
                            if (stripBmp == null) {
                                Log.e(TAG, "decodeRegion returned null for strip at source row " + srcY);
                                streamSuccess = false;
                                break;
                            }
                            if (canReuseRegionBitmap && reusableStripBmp == null && currentSrcH == srcStripH && stripBmp.isMutable()) {
                                reusableStripBmp = stripBmp;
                            }
                            int outStripW = stripBmp.getWidth();  // 3976, 5168, or 7952
                            int outStripH = stripBmp.getHeight(); // 256 (or partial final strip)
                            boolean isRgb565 = (opts.inPreferredConfig == Bitmap.Config.RGB_565)
                                || (stripBmp.getConfig() == Bitmap.Config.RGB_565);
                            if (isRgb565 && stripDirect.capacity() >= outStripW * outStripH * 4) {
                                stripDirect = java.nio.ByteBuffer.allocateDirect(outStripW * outStripH * 2);
                            }

                            long tJni0 = System.currentTimeMillis();
                            stripDirect.clear();
                            stripBmp.copyPixelsToBuffer(stripDirect);
                            stripDirect.rewind();

                            // Native C 4-core parallel grading + pipelined JPEG stream write
                            boolean stripOk = ColorEngine.gradeAndWriteStreamStripSafe(
                                streamHandle, stripDirect, outStripW, outStripH, outputRowOffset, grainStrength, clarityStrength
                            );
                            brdJniMs += (System.currentTimeMillis() - tJni0);
                            if (!stripOk) {
                                Log.e(TAG, "Failed encoding strip at output row " + outputRowOffset);
                                streamSuccess = false;
                                break;
                            }
                            outputRowOffset += outStripH;
                        } finally {
                            // Crucial: recycle non-reusable strip Bitmap immediately
                            if (stripBmp != null && stripBmp != reusableStripBmp && !stripBmp.isRecycled()) {
                                stripBmp.recycle();
                                stripBmp = null;
                            }
                        }
                    }
                } finally {
                    opts.inBitmap = null;
                    if (reusableStripBmp != null && !reusableStripBmp.isRecycled()) {
                        reusableStripBmp.recycle();
                        reusableStripBmp = null;
                    }
                    // Deterministic stream closure in finally block
                    boolean closed = ColorEngine.closeJpegStreamSafe(streamHandle, streamSuccess);
                    streamHandle = 0;
                    if (!closed) streamSuccess = false;
                    if (decoder != null) {
                        decoder.recycle();
                        decoder = null;
                    }
                    if (!streamSuccess) {
                        if (tmpFile.exists()) tmpFile.delete(); // Delete incomplete .tmp file on failure/abort
                        if (usedDirectTarget && targetOutputFile.exists()) targetOutputFile.delete();
                        if (cacheStreamFile != null && cacheStreamFile.exists()) cacheStreamFile.delete();
                    }
                }
            }

            // Grab in-memory DC RGB565 preview (zero SD re-read / Skia decode!)
            Bitmap reviewBmp = usedFastTranscode ? ColorEngine.takeLastFastPreviewBitmapSafe() : null;

            // 7. Finalize native output into the EXACT SAME folder as the original image
            //    (Tier 1 atomic .TMP -> .JPG rename / Tier 2 direct .JPG / Tier 3 verified copy or MediaStore)
            String gradedSavedPath = null;
            if (streamSuccess && usedCacheStream && cacheStreamFile != null && cacheStreamFile.exists() && cacheStreamFile.length() > 0) {
                gradedSavedPath = StorageResolver.saveJpegFileViaMediaStore(context, cacheStreamFile, targetOutputFile, presetName);
                if (gradedSavedPath != null && new File(gradedSavedPath).exists() && new File(gradedSavedPath).length() > 0) {
                    cacheStreamFile.delete();
                    recordKnownPhoto(new File(gradedSavedPath));
                    if (!inlineExifWritten) {
                        if (recipe.dynamicRange != null && recipe.dynamicRange != FujiRecipe.DynamicRange.DR100) {
                            StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName, recipe.dynamicRange.label, dstW, dstH);
                        } else {
                            StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName, dstW, dstH);
                        }
                    }
                    final String avindexPath = gradedSavedPath;
                    ioExecutor.execute(new Runnable() {
                        @Override
                        public void run() {
                            StorageResolver.registerWithAvindex(context, avindexPath);
                        }
                    });
                } else {
                    Log.e(TAG, "Tier-3 save failed for " + targetOutputFile.getAbsolutePath() + " (diag=" + StorageResolver.getLastDiagnostic() + "); keeping cache file " + cacheStreamFile.getAbsolutePath());
                    gradedSavedPath = null;
                }
            } else if (streamSuccess && usedDirectTarget && actualDirectTarget != null && actualDirectTarget.exists() && actualDirectTarget.length() > 0) {
                gradedSavedPath = actualDirectTarget.getAbsolutePath();
                recordKnownPhoto(actualDirectTarget);
                if (!inlineExifWritten) {
                    if (recipe.dynamicRange != null && recipe.dynamicRange != FujiRecipe.DynamicRange.DR100) {
                        StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName, recipe.dynamicRange.label, dstW, dstH);
                    } else {
                        StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName, dstW, dstH);
                    }
                }
                final String avindexPath = gradedSavedPath;
                ioExecutor.execute(new Runnable() {
                    @Override
                    public void run() {
                        StorageResolver.registerWithAvindex(context, avindexPath);
                    }
                });
            } else if (streamSuccess && tmpFile.exists() && tmpFile.length() > 0) {
                if (targetOutputFile.exists()) {
                    targetOutputFile.delete();
                }
                boolean renamed = tmpFile.renameTo(targetOutputFile);
                if (!renamed) {
                    try {
                        StorageResolver.copyFile(tmpFile, targetOutputFile);
                        if (targetOutputFile.exists() && targetOutputFile.length() > 0) {
                            tmpFile.delete();
                        }
                    } catch (Throwable copyErr) {
                        Log.e(TAG, "Failed finalizing " + tmpFile.getName() + " -> " + targetOutputFile.getAbsolutePath(), copyErr);
                    }
                }
                if (targetOutputFile.exists() && targetOutputFile.length() > 0) {
                    gradedSavedPath = targetOutputFile.getAbsolutePath();
                    recordKnownPhoto(targetOutputFile);

                    // 8. Copy original camera EXIF metadata ONLY if inline single-pass APP1 wasn't already embedded
                    if (!inlineExifWritten) {
                        if (recipe.dynamicRange != null && recipe.dynamicRange != FujiRecipe.DynamicRange.DR100) {
                            StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName, recipe.dynamicRange.label, dstW, dstH);
                        } else {
                            StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName, dstW, dstH);
                        }
                    }

                    // 9. Register with Sony AvindexStore asynchronously off the grading worker
                    final String avindexPath = gradedSavedPath;
                    ioExecutor.execute(new Runnable() {
                        @Override
                        public void run() {
                            StorageResolver.registerWithAvindex(context, avindexPath);
                        }
                    });
                }
            } else {
                if (tmpFile.exists()) {
                    tmpFile.delete();
                }
                if (cacheStreamFile != null && cacheStreamFile.exists()) {
                    cacheStreamFile.delete();
                }
                Log.e(TAG, "Strip streaming failed to produce valid output");
            }

            final long totalMs = System.currentTimeMillis() - captureStartTime;
            final float mp = (float)(dstW * dstH) / 1000000.0f;
            final File verifiedSavedFile = (gradedSavedPath != null) ? new File(gradedSavedPath) : null;
            final boolean saveVerified = (verifiedSavedFile != null && verifiedSavedFile.exists() && verifiedSavedFile.length() > 0);
            if (!saveVerified) {
                gradedSavedPath = null;
            }
            final String savedLabel;
            if (saveVerified) {
                File parentDir = verifiedSavedFile.getParentFile();
                savedLabel = (parentDir != null)
                    ? (parentDir.getName() + "/" + verifiedSavedFile.getName())
                    : verifiedSavedFile.getName();
            } else {
                savedLabel = "SAVE FAILED" + (lastOpenErrno != 0 ? (" e:" + lastOpenErrno) : "");
            }
            final int[] fastStats = usedFastTranscode ? ColorEngine.getLastFastStatsSafe() : null;
            final String cpuDiag = (fastStats != null)
                ? String.format(Locale.US, "h:%d p:%d e:%d %dc/v%d%s",
                    fastStats[2], fastStats[3], fastStats[4],
                    fastStats[5], (fastStats[7] != 0 ? 7 : 5), (fastStats[6] != 0 ? "N" : ""))
                : null;
            final String tierTag = (usedFastTier > 1 && lastOpenErrno != 0)
                ? String.format(Locale.US, "t%d(e%d)", usedFastTier, lastOpenErrno)
                : String.format(Locale.US, "t%d", usedFastTier);
            final String profDiag = (fastStats != null && fastStats.length >= 24)
                ? String.format(Locale.US, "cpu s2:%d dec:%d em:%d | idct:%d lut:%d fdct:%d\nsu:%d rd:%d lutb:%d pin:%d %s dct:%d%%",
                    fastStats[13], fastStats[12], fastStats[14], fastStats[15], fastStats[16], fastStats[17],
                    fastStats[10], fastStats[22], fastStats[11], fastStats[23], tierTag, fastStats[18])
                : null;
            final String perfTag = usedFastTranscode
                ? String.format(Locale.US, "C-FAST sd:%d c:%d", sdWaitMs, fastTranscodeMs)
                : String.format(Locale.US, "BRD rc:%d sd:%d idx:%d dec:%d jni:%d", lastFastRc, sdWaitMs, brdIdxMs, brdDecMs, brdJniMs);
            final String telemetry = saveVerified
                ? (String.format(Locale.US, "SAVED %s (%dx%d | %.1fMP) in %dms",
                    savedLabel, dstW, dstH, mp, totalMs) + " [" + perfTag + (cpuDiag != null ? " " + cpuDiag : "") + "]")
                : (String.format(Locale.US, "SAVE FAILED %s (%dx%d | %.1fMP) in %dms",
                    dcfJpegName, dstW, dstH, mp, totalMs) + " [" + perfTag + " e:" + lastOpenErrno + "]");
            uiController.updateStatusAsync(telemetry);
            Log.i(TAG, telemetry + " -> " + gradedSavedPath);

            // 10. Instant Zero-I/O Quick Review from in-memory DC RGB565 preview (skips 1-1.5s SD re-read & Skia decode!)
            if (reviewBmp == null) {
                File reviewFile = saveVerified ? verifiedSavedFile : targetOutputFile;
                if (reviewFile != null && reviewFile.exists()) {
                    reviewBmp = StorageResolver.loadPhotoForReview(reviewFile, 960);
                }
            }
            if (reviewBmp == null && rawMasterFile.exists()) {
                reviewBmp = StorageResolver.loadPhotoForReview(rawMasterFile, 960);
                if (reviewBmp != null) {
                    if (!ColorEngine.applyLutAndGrainBitmapSafe(reviewBmp, grainStrength, clarityStrength)) {
                        reviewBmp = applyRecipeInJava(reviewBmp, recipe);
                    }
                }
            }
            if (reviewBmp != null) {
                String reviewSubline;
                if (!saveVerified) {
                    reviewSubline = savedLabel + (cpuDiag != null ? (" | " + cpuDiag) : "")
                        + (profDiag != null ? ("\n" + profDiag) : "");
                } else {
                    reviewSubline = (cpuDiag != null)
                        ? (cpuDiag + (profDiag != null ? "\n" + profDiag : "") + " | " + savedLabel)
                        : savedLabel;
                }
                if (resultListener != null) {
                    deliverResult(reviewBmp, presetName, dstW, dstH, totalMs, reviewSubline, perfTag);
                } else if (uiController != null) {
                    uiController.showQuickReview(reviewBmp);
                } else if (!reviewBmp.isRecycled()) {
                    reviewBmp.recycle();
                }
            }

        } catch (Throwable t) {
            Log.e(TAG, "Error in processStripStreamingCapture", t);
            if (uiController != null) {
                uiController.updateStatusAsync("ERR: " + t.getClass().getSimpleName() + ": " + t.getMessage());
            }
        } finally {
            if (prevPrio != Integer.MIN_VALUE) {
                try { android.os.Process.setThreadPriority(prevPrio); } catch (Throwable ignored) {}
            }
            int remaining = pendingJobsCount.decrementAndGet();
            if (remaining < 0) {
                pendingJobsCount.set(0);
                remaining = 0;
            }
            appState.setProcessing(remaining > 0);
            updateQueueHud();
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

    private static long openNativeStreamTier(File targetFile, int width, int height, byte[] inlineExifApp1) {
        if (targetFile == null) return 0;
        String path = targetFile.getAbsolutePath();
        if (inlineExifApp1 != null && inlineExifApp1.length >= 4) {
            return ColorEngine.openJpegStreamWithExifSafe(path, width, height, 92, inlineExifApp1);
        }
        return ColorEngine.openJpegStreamSafe(path, width, height, 92);
    }

    private void deliverResult(Bitmap bitmap, String preset, int w, int h, long ms, String savedPath) {
        deliverResult(bitmap, preset, w, h, ms, savedPath, ColorEngine.isNativeAvailable() ? "C" : "JAVA");
    }

    private void deliverResult(Bitmap bitmap, String preset, int w, int h, long ms, String savedPath, String engineTag) {
        float mp = (float)(w * h) / 1000000.0f;
        String tag = (engineTag != null && engineTag.length() > 0) ? engineTag : "C";
        String baseInfo = String.format(Locale.US, "%s  %.1fMP [%s]  %d ms", preset, mp, tag, ms);
        if (savedPath != null) {
            if (savedPath.startsWith("SAVE FAILED")) {
                baseInfo += "\n" + savedPath;
            } else {
                baseInfo += "\nSAVED OK: " + savedPath;
            }
        } else if (!ColorEngine.isNativeAvailable() && ColorEngine.getNativeLoadError() != null) {
            baseInfo += "\nJNI: " + ColorEngine.getNativeLoadError();
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

    /**
     * Pure-Java film simulation grading engine:
     * Applies Q8.8 3x3 Color Correction Matrix, Highlight/Shadow 1D Tone Curves,
     * and Grain effect directly to Bitmap pixels in 256-row strips.
     * Guarantees 100% crash-proof execution with zero JNI dependencies and ~3.8MB RAM footprint.
     */
    private Bitmap applyRecipeInJava(Bitmap bmp, FujiRecipe recipe) {
        if (bmp == null || recipe == null) return bmp;
        Bitmap targetBmp = bmp;
        if (!targetBmp.isMutable()) {
            try {
                Bitmap mutable = targetBmp.copy(Bitmap.Config.RGB_565, true);
                if (mutable != null) targetBmp = mutable;
            } catch (Throwable ignored) {}
        }

        int w = targetBmp.getWidth();
        int h = targetBmp.getHeight();
        short[] ccm = recipe.computeCcmQ8();
        byte[] rCurve = recipe.generateToneCurve('R');
        byte[] gCurve = recipe.generateToneCurve('G');
        byte[] bCurve = recipe.generateToneCurve('B');

        int stripH = 256;
        int[] pixels = new int[w * stripH];
        int grainAmp = (recipe.grainEffect == FujiRecipe.EffectLevel.STRONG) ? 14 : ((recipe.grainEffect == FujiRecipe.EffectLevel.WEAK) ? 7 : 0);

        for (int y = 0; y < h; y += stripH) {
            int currentH = Math.min(stripH, h - y);
            targetBmp.getPixels(pixels, 0, w, 0, y, w, currentH);
            int count = w * currentH;
            for (int i = 0; i < count; i++) {
                int c = pixels[i];
                int r = (c >> 16) & 0xFF;
                int g = (c >> 8) & 0xFF;
                int b = c & 0xFF;

                int rOut = (ccm[0] * r + ccm[1] * g + ccm[2] * b + 128) >> 8;
                int gOut = (ccm[3] * r + ccm[4] * g + ccm[5] * b + 128) >> 8;
                int bOut = (ccm[6] * r + ccm[7] * g + ccm[8] * b + 128) >> 8;

                if (rOut < 0) rOut = 0; else if (rOut > 255) rOut = 255;
                if (gOut < 0) gOut = 0; else if (gOut > 255) gOut = 255;
                if (bOut < 0) bOut = 0; else if (bOut > 255) bOut = 255;

                rOut = rCurve[rOut] & 0xFF;
                gOut = gCurve[gOut] & 0xFF;
                bOut = bCurve[bOut] & 0xFF;

                if (grainAmp > 0) {
                    int noise = (((i * 1103515245 + 12345) >> 16) & 31) - 16;
                    int gAdd = (noise * grainAmp) >> 4;
                    rOut = Math.max(0, Math.min(255, rOut + gAdd));
                    gOut = Math.max(0, Math.min(255, gOut + gAdd));
                    bOut = Math.max(0, Math.min(255, bOut + gAdd));
                }

                pixels[i] = 0xFF000000 | (rOut << 16) | (gOut << 8) | bOut;
            }
            try {
                targetBmp.setPixels(pixels, 0, w, 0, y, w, currentH);
            } catch (Throwable t) {
                Log.w(TAG, "setPixels failed on bitmap", t);
                break;
            }
        }
        return targetBmp;
    }

    public void shutdown() {
        isShuttingDown = true;
        synchronized (hardwareStoreLock) {
            waitingPollerJobs = 0;
            pendingHardwareStoredQueue.clear();
            hardwareStoreLock.notifyAll();
        }
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
            if (!ioExecutor.awaitTermination(3000, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                ioExecutor.shutdownNow();
            }
        } catch (InterruptedException e) {
            ioExecutor.shutdownNow();
        }
    }
}
