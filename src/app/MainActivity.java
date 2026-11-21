package com.fujilite.a7r2;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;

/**
 * Main Activity for Sony A7R II (ILCE-7RM2) running Fuji-Lite.
 *
 * Enforces:
 *  - Zero Android Buttons or focusable widgets (eliminates focus stealing).
 *  - 2-Layer Fullscreen Architecture (SurfaceView underneath, FujiQMenuView unified HUD on top).
 *  - Mandatory Activity.dispatchKeyEvent() intercepting scanCode >= 512 and keyCode == 0
 *    for all 18 buttons and 3 physical dials on Sony A7R II.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {

    private static final String TAG = "FujiLiteMain";

    private FrameLayout rootLayout;
    private SurfaceView cameraPreview;
    private FujiQMenuView qMenuView;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final java.util.concurrent.ExecutorService previewExecutor =
        java.util.concurrent.Executors.newSingleThreadExecutor(new java.util.concurrent.ThreadFactory() {
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
                }, "FujiLite-LiveLUT");
                t.setPriority(Thread.NORM_PRIORITY - 1);
                return t;
            }
        });
    private final java.util.concurrent.atomic.AtomicBoolean previewFrameBusy =
        new java.util.concurrent.atomic.AtomicBoolean(false);
    private volatile long lastPreviewProcessTime = 0;
    private static final long MIN_PREVIEW_INTERVAL_MS = 100L; // Cap software live-view LUT at <=10 FPS
    private volatile int cachedPreviewWidth = 0;
    private volatile int cachedPreviewHeight = 0;
    private volatile boolean surfaceReady = false;

    private AppState appState;
    private UIController uiController;
    private SonyPmcaAdapter cameraAdapter;
    private CaptureController captureController;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread thread, Throwable ex) {
                Log.e(TAG, "FATAL CRASH on thread " + thread.getName(), ex);
            }
        });

        try {
            buildUi();
            initPipeline();
        } catch (Throwable t) {
            Log.e(TAG, "Startup failed; continuing in degraded mode", t);
        }
    }

    private void buildUi() {
        rootLayout = new FrameLayout(this);
        rootLayout.setBackgroundColor(Color.BLACK);
        rootLayout.setFocusable(true);
        rootLayout.setFocusableInTouchMode(true);

        // Layer 0: Camera Live View SurfaceView (Non-focusable)
        cameraPreview = new SurfaceView(this);
        cameraPreview.setFocusable(false);
        cameraPreview.setFocusableInTouchMode(false);
        try {
            cameraPreview.getHolder().setType(SurfaceHolder.SURFACE_TYPE_PUSH_BUFFERS);
        } catch (Throwable ignored) {}

        rootLayout.addView(cameraPreview, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        setContentView(rootLayout);
    }

    private void initPipeline() {
        appState = new AppState();
        cameraAdapter = new SonyPmcaAdapter();
        uiController = new UIController(null);
        uiController.setCameraAdapter(cameraAdapter);

        // Load saved recipe slot 0 (C1)
        FujiRecipe recipe = appState.getActiveRecipe();
        recipe.loadFromSlot(this, 0);

        // Layer 1: Unified FujiQMenuView overlay (Live View HUD, 4x4 Q-Menu, Quick Review)
        qMenuView = new FujiQMenuView(this, recipe, new FujiQMenuView.OnRecipeChangeListener() {
            @Override
            public void onRecipeChanged(FujiRecipe r) {
                onRecipeUpdated(r);
            }

            @Override
            public void onDismissRequest() {
                if (qMenuView != null) {
                    qMenuView.setQMenuOpen(false);
                    if (appState != null) appState.setQMenuVisible(false);
                    ui.removeCallbacks(applyHardwareParamsRunnable);
                    applyHardwareParamsRunnable.run();
                    if (cameraAdapter != null) cameraAdapter.resumePreview();
                }
            }
        });
        qMenuView.setAppState(appState);
        qMenuView.setFocusable(false);
        qMenuView.setFocusableInTouchMode(false);

        rootLayout.addView(qMenuView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        uiController.setQMenuView(qMenuView);

        uiController.setControllerCallback(new UIController.ControllerCallback() {
            @Override
            public void onTriggerCapture() {
                if (captureController != null) captureController.triggerCapture();
            }

            @Override
            public void onToggleQMenu() {
                if (qMenuView != null) {
                    boolean target = !qMenuView.isQMenuOpen();
                    qMenuView.setQMenuOpen(target);
                    if (appState != null) appState.setQMenuVisible(target);
                    if (!target) {
                        ui.removeCallbacks(applyHardwareParamsRunnable);
                        applyHardwareParamsRunnable.run();
                        if (cameraAdapter != null) cameraAdapter.resumePreview();
                    }
                }
            }

            @Override
            public boolean isQMenuOpen() {
                return qMenuView != null && qMenuView.isQMenuOpen();
            }

            @Override
            public void onCycleFilmSim(int direction) {
                cycleLook(direction);
            }

            @Override
            public void onCycleDynamicRange() {
                FujiRecipe r = appState.getActiveRecipe();
                int next = (r.dynamicRange.ordinal() + 1) % FujiRecipe.DynamicRange.values().length;
                r.dynamicRange = FujiRecipe.DynamicRange.values()[next];
                onRecipeUpdated(r);
            }

            @Override
            public void onCycleRecipeSlot() {
                FujiRecipe r = appState.getActiveRecipe();
                int next = (r.recipeSlotIndex + 1) % FujiRecipe.SLOT_COUNT;
                r.loadFromSlot(MainActivity.this, next);
                onRecipeUpdated(r);
            }

            @Override
            public void onCycleGrain() {
                FujiRecipe r = appState.getActiveRecipe();
                int next = (r.grainEffect.ordinal() + 1) % FujiRecipe.EffectLevel.values().length;
                r.grainEffect = FujiRecipe.EffectLevel.values()[next];
                if (qMenuView != null) {
                    qMenuView.showCarouselBanner("GRAIN: " + r.grainEffect.label);
                }
                onRecipeUpdated(r);
            }

            @Override
            public void onToggleAel() {
                if (cameraAdapter != null) {
                    final boolean targetLock = !cameraAdapter.isAutoExposureLock();
                    cameraAdapter.postHardwareTask(new Runnable() {
                        @Override
                        public void run() {
                            if (cameraAdapter != null) {
                                cameraAdapter.setAutoExposureLock(targetLock);
                                ui.post(new Runnable() {
                                    @Override
                                    public void run() {
                                        if (uiController != null) uiController.updateBottomHud();
                                    }
                                });
                            }
                        }
                    });
                }
            }

            @Override
            public void onToggleAfMode() {
                if (cameraAdapter != null) {
                    cameraAdapter.postHardwareTask(new Runnable() {
                        @Override
                        public void run() {
                            if (cameraAdapter != null) {
                                cameraAdapter.toggleAfMode();
                                ui.post(new Runnable() {
                                    @Override
                                    public void run() {
                                        if (uiController != null) uiController.updateBottomHud();
                                    }
                                });
                            }
                        }
                    });
                }
            }

            @Override
            public void onToggleLiveViewLut() {
                boolean enabled = uiController.toggleLiveViewLut();
                if (!enabled && qMenuView != null) {
                    qMenuView.clearLiveViewBuffer();
                }
                uiController.updateStatusAsync(enabled ? "LUT Preview ON" : "LUT Preview OFF");
            }

            @Override
            public void onToggleQuickReview() {
                if (qMenuView != null) {
                    if (qMenuView.isReviewing()) {
                        qMenuView.clearQuickReview();
                        if (cameraAdapter != null) cameraAdapter.resumePreview();
                        return;
                    }

                    // Offload SD card scan + JPEG decode off the Main UI Thread
                    previewExecutor.execute(new Runnable() {
                        @Override
                        public void run() {
                            final java.io.File latest = StorageResolver.getLatestSavedPhoto();
                            if (latest == null || !latest.exists()) {
                                if (uiController != null) {
                                    uiController.updateStatusAsync("No photos found in DCIM");
                                }
                                return;
                            }

                            // Attempt 1: Native Sony Scalar Review Intent (dispatched on UI thread)
                            final android.content.Intent reviewIntent = new android.content.Intent("com.sony.scalar.intent.action.REVIEW");
                            reviewIntent.setDataAndType(android.net.Uri.fromFile(latest), "image/jpeg");

                            // Pre-decode display-safe fallback bitmap off the UI thread
                            final Bitmap bmp = StorageResolver.loadPhotoForReview(latest, 640);
                            ui.post(new Runnable() {
                                @Override
                                public void run() {
                                    try {
                                        startActivity(reviewIntent);
                                        if (bmp != null && !bmp.isRecycled()) bmp.recycle();
                                        return;
                                    } catch (Throwable t) {
                                        Log.d(TAG, "Native Sony review intent unavailable; using in-app review: " + t.getMessage());
                                    }
                                    if (bmp != null && qMenuView != null) {
                                        qMenuView.setQuickReviewBitmap(bmp, "REPLAY: " + latest.getName() + "\nPRESS PLAY / SHUTTER TO EXIT");
                                    } else if (uiController != null) {
                                        uiController.updateStatusAsync("No photos found in DCIM");
                                    }
                                }
                            });
                        }
                    });
                }
            }

            @Override
            public void onResetParameter() {
                if (cameraAdapter != null) {
                    cameraAdapter.postHardwareTask(new Runnable() {
                        @Override
                        public void run() {
                            if (cameraAdapter != null) {
                                cameraAdapter.incrementEV(0);
                                ui.post(new Runnable() {
                                    @Override
                                    public void run() {
                                        if (uiController != null) uiController.updateBottomHud();
                                    }
                                });
                            }
                        }
                    });
                }
            }

            @Override
            public void onExitApp() {
                finish();
            }
        });

        captureController = new CaptureController(this, cameraAdapter, uiController, appState);

        captureController.setResultListener(new CaptureController.ResultListener() {
            @Override
            public void onResult(final Bitmap graded, final String info) {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        if (qMenuView != null) {
                            qMenuView.setQuickReviewBitmap(graded, info);
                        }
                    }
                });
            }
        });

        cameraPreview.getHolder().addCallback(this);

        // Precompute initial 17x17x17 LUT off the UI thread so startup never blocks
        previewExecutor.execute(new Runnable() {
            @Override
            public void run() {
                precomputeActiveLut();
            }
        });
        uiController.updateBottomHud();
    }

    private final Runnable applyHardwareParamsRunnable = new Runnable() {
        @Override
        public void run() {
            if (cameraAdapter != null) {
                // Execute Camera.setParameters() off the Main UI thread to prevent BIONZ X ISP UI stalls
                cameraAdapter.postHardwareTask(new Runnable() {
                    @Override
                    public void run() {
                        if (cameraAdapter != null && appState != null) {
                            SonyToFujiHardwareMapper.applyRecipeToHardware(
                                cameraAdapter.getCameraEx(),
                                cameraAdapter.getNormalCamera(),
                                appState.getActiveRecipe()
                            );
                        }
                    }
                });
            }
        }
    };

    private void onRecipeUpdated(FujiRecipe r) {
        if (qMenuView != null) {
            qMenuView.setRecipe(r);
            qMenuView.showCarouselBanner(r.filmSim.label);
        }

        // 1. Stage 1: Pre-compute 17x17x17 LUT off the Main UI thread (zero asset/SD I/O on UI thread)
        previewExecutor.execute(new Runnable() {
            @Override
            public void run() {
                precomputeActiveLut();
            }
        });
        uiController.updateBottomHud();

        // 2. Stage 2: Hardware mapping to Sony ISP (Debounced during Q-Menu interaction and executed off UI thread)
        ui.removeCallbacks(applyHardwareParamsRunnable);
        if (qMenuView != null && qMenuView.isQMenuOpen()) {
            ui.postDelayed(applyHardwareParamsRunnable, 300);
        } else {
            applyHardwareParamsRunnable.run();
        }
    }

    private void precomputeActiveLut() {
        FujiRecipe r = appState.getActiveRecipe();
        byte[] baseLut = LutManager.getLutTable(this, r.filmSim.lutFile);
        byte[] curve = r.generateToneCurve('R');
        if (baseLut != null) {
            short[] adjCcm = r.computeAdjustmentCcmQ8();
            ColorEngine.precomputeLut17WithBaseSafe(baseLut, adjCcm, curve, curve, curve);
        } else {
            short[] ccm = r.computeCcmQ8();
            ColorEngine.precomputeLut17Safe(ccm, curve, curve, curve);
        }
    }

    private void cycleLook(int delta) {
        FujiRecipe recipe = appState.getActiveRecipe();
        int len = FujiRecipe.FilmSim.values().length;
        int next = (recipe.filmSim.ordinal() + delta + len) % len;
        recipe.filmSim = FujiRecipe.FilmSim.values()[next];
        onRecipeUpdated(recipe);
    }

    private boolean isS2PhysicallyHeld = false;
    private long lastShutterDownTime = 0;
    private boolean isS1Held = false;

    /**
     * Critical PMCA Key & Dial Interceptor.
     * Intercepts all physical buttons and dials on Sony A7R II before any Android view focus logic.
     * Prioritizes scanCode whenever scanCode >= 512 or keyCode == 0.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int scanCode = event.getScanCode();
        int keyCode = event.getKeyCode();

        // On Sony A7R II, dials and buttons have scanCode >= 512 or keyCode == 0
        int rawCode = (scanCode >= 512 || keyCode == 0) ? scanCode : keyCode;

        int action = event.getAction();
        SonyKeyCodes.FujiAction fujiAction = SonyKeyCodes.resolveAction(rawCode, event);

        // Strict Hardware Shutter Debouncing (catches 514, 0x202, 513, 0x201)
        if (fujiAction == SonyKeyCodes.FujiAction.FULL_SHUTTER) {
            if (action == KeyEvent.ACTION_DOWN) {
                long now = System.currentTimeMillis();
                if (event.getRepeatCount() > 0) return true;
                if (now - lastShutterDownTime < 600) return true; // Debounce 600ms
                isS2PhysicallyHeld = true;
                lastShutterDownTime = now;

                // Seamless dismissal: if reviewing or in Q-Menu, exit overlay immediately to shoot
                if (qMenuView != null && qMenuView.isReviewing()) {
                    qMenuView.clearQuickReview();
                    if (cameraAdapter != null) cameraAdapter.resumePreview();
                }
                if (qMenuView != null && qMenuView.isQMenuOpen()) {
                    qMenuView.setQMenuOpen(false);
                    if (appState != null) appState.setQMenuVisible(false);
                    ui.removeCallbacks(applyHardwareParamsRunnable);
                    applyHardwareParamsRunnable.run();
                    if (cameraAdapter != null) cameraAdapter.resumePreview();
                }

                if (captureController != null) captureController.triggerCapture();
                return true;
            } else if (action == KeyEvent.ACTION_UP) {
                isS2PhysicallyHeld = false;
                if (captureController != null) captureController.onShutterKeyUp();
                return true;
            }
        }

        if (fujiAction == SonyKeyCodes.FujiAction.HALF_SHUTTER) {
            if (action == KeyEvent.ACTION_DOWN) {
                isS2PhysicallyHeld = false; // Reset S2 state on new focus half-press
                if (event.getRepeatCount() > 0 || isS1Held) return true;
                isS1Held = true;

                // Half-press dismisses review or Q-Menu immediately back to Live View
                if (qMenuView != null && qMenuView.isReviewing()) {
                    qMenuView.clearQuickReview();
                    if (cameraAdapter != null) cameraAdapter.resumePreview();
                }
                if (qMenuView != null && qMenuView.isQMenuOpen()) {
                    qMenuView.setQMenuOpen(false);
                    if (appState != null) appState.setQMenuVisible(false);
                    ui.removeCallbacks(applyHardwareParamsRunnable);
                    applyHardwareParamsRunnable.run();
                    if (cameraAdapter != null) cameraAdapter.resumePreview();
                }

                if (cameraAdapter instanceof SonyPmcaAdapter) {
                    ((SonyPmcaAdapter) cameraAdapter).autoFocus();
                }
                return true;
            } else if (action == KeyEvent.ACTION_UP) {
                isS1Held = false;
                if (cameraAdapter instanceof SonyPmcaAdapter) {
                    ((SonyPmcaAdapter) cameraAdapter).cancelAutoFocus();
                }
                return true;
            }
        }

        if (action == KeyEvent.ACTION_DOWN) {
            if (uiController != null && uiController.handleRawKeyDown(rawCode, event)) {
                return true;
            }
        } else if (action == KeyEvent.ACTION_UP) {
            // Failsafe for dials that only emit ACTION_UP on some firmware versions
            if (rawCode >= 519 && rawCode <= 537) {
                 if (uiController != null && uiController.handleRawKeyDown(rawCode, event)) return true;
            }
            if (uiController != null && uiController.handleRawKeyUp(rawCode, event)) {
                return true;
            }
        }

        return super.dispatchKeyEvent(event);
    }

    private final android.hardware.Camera.PreviewCallback realtimePreviewCallback = new android.hardware.Camera.PreviewCallback() {
        @Override
        public void onPreviewFrame(final byte[] data, android.hardware.Camera camera) {
            if (data == null) return;
            final int w = cachedPreviewWidth;
            final int h = cachedPreviewHeight;
            long now = System.currentTimeMillis();

            // Zero Binder IPC on preview frames; throttle <= 10 FPS & drop frames if busy/capturing/in menu
            boolean shouldProcess = (w > 0 && h > 0)
                && qMenuView != null
                && uiController != null
                && uiController.isLiveViewLutEnabled()
                && !qMenuView.isQMenuOpen()
                && !qMenuView.isReviewing()
                && (appState == null || !appState.isProcessing())
                && (now - lastPreviewProcessTime >= MIN_PREVIEW_INTERVAL_MS)
                && previewFrameBusy.compareAndSet(false, true);

            if (!shouldProcess) {
                if (cameraAdapter instanceof SonyPmcaAdapter) {
                    ((SonyPmcaAdapter) cameraAdapter).addCallbackBuffer(data);
                }
                return;
            }

            lastPreviewProcessTime = now;
            final int grainLevel = (appState != null) ? appState.getActiveRecipe().grainEffect.ordinal() : 0;

            // Offload 3D LUT grading + Bitmap pixel copy completely off the Main UI Thread
            try {
                previewExecutor.execute(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            if (qMenuView != null && !qMenuView.isQMenuOpen() && !qMenuView.isReviewing()) {
                                qMenuView.processLiveViewFrame(data, w, h, grainLevel);
                            }
                        } finally {
                            previewFrameBusy.set(false);
                            if (cameraAdapter instanceof SonyPmcaAdapter) {
                                ((SonyPmcaAdapter) cameraAdapter).addCallbackBuffer(data);
                            }
                        }
                    }
                });
            } catch (Throwable t) {
                previewFrameBusy.set(false);
                if (cameraAdapter instanceof SonyPmcaAdapter) {
                    ((SonyPmcaAdapter) cameraAdapter).addCallbackBuffer(data);
                }
            }
        }
    };

    private void attachRealtimePreviewCallback() {
        if (cameraAdapter instanceof SonyPmcaAdapter) {
            try {
                android.hardware.Camera normalCam = cameraAdapter.getNormalCamera();
                if (normalCam != null) {
                    android.hardware.Camera.Size previewSize = normalCam.getParameters().getPreviewSize();
                    if (previewSize != null) {
                        cachedPreviewWidth = previewSize.width;
                        cachedPreviewHeight = previewSize.height;
                        ((SonyPmcaAdapter) cameraAdapter).setRealtimePreviewCallback(
                            realtimePreviewCallback, previewSize.width, previewSize.height);
                    }
                }
            } catch (Throwable t) {
                Log.e(TAG, "Failed to initialize realtime preview callback", t);
            }
        }
    }

    private void startPollerIfReady() {
        if (cameraAdapter instanceof SonyPmcaAdapter) {
            ((SonyPmcaAdapter) cameraAdapter).startHardwarePoller(new Runnable() {
                @Override
                public void run() {
                    if (uiController != null) uiController.updateBottomHud();
                }
            });
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        try {
            surfaceReady = true;
            if (cameraAdapter == null) return;
            cameraAdapter.startPreview(holder);

            // Apply initial recipe parameters off the Main UI Thread
            applyHardwareParamsRunnable.run();

            if (uiController != null) {
                uiController.updateBottomHud();
            }

            // Cache preview dimensions ONCE here (eliminates 30Hz camera.getParameters() IPC in onPreviewFrame)
            attachRealtimePreviewCallback();
            startPollerIfReady();
        } catch (Throwable t) {
            Log.e(TAG, "surfaceCreated failed", t);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        cachedPreviewWidth = 0;
        cachedPreviewHeight = 0;
        if (cameraAdapter != null) {
            cameraAdapter.release();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        try {
            // Only restart preview when surface is already created (avoids cold-start double open & restores preview after Review Intent)
            if (surfaceReady && cameraAdapter != null && cameraPreview != null) {
                cameraAdapter.startPreview(cameraPreview.getHolder());
                applyHardwareParamsRunnable.run();
                attachRealtimePreviewCallback();
                startPollerIfReady();
            }
        } catch (Throwable t) {
            Log.e(TAG, "onResume startPreview failed", t);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        ui.removeCallbacks(applyHardwareParamsRunnable);
        if (cameraAdapter != null) {
            cameraAdapter.stopHardwarePoller();
            cameraAdapter.release();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ui.removeCallbacks(applyHardwareParamsRunnable);
        previewExecutor.shutdownNow();
        if (captureController != null) {
            captureController.shutdown();
        }
        if (cameraAdapter != null) {
            cameraAdapter.shutdownPollerThread();
            cameraAdapter.release();
            cameraAdapter = null;
        }
    }
}
