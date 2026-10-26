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

    private AppState appState;
    private UIController uiController;
    private SonyPmcaAdapter cameraAdapter;
    private CaptureController captureController;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

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
                if (qMenuView != null) qMenuView.setQMenuOpen(false);
            }
        });
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
                    qMenuView.setQMenuOpen(!qMenuView.isQMenuOpen());
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
                    cameraAdapter.setAutoExposureLock(!cameraAdapter.isAutoExposureLock());
                }
                uiController.updateBottomHud();
            }

            @Override
            public void onToggleAfMode() {
                if (cameraAdapter != null) {
                    cameraAdapter.toggleAfMode();
                }
                uiController.updateBottomHud();
            }

            @Override
            public void onToggleLiveViewLut() {
                uiController.updateStatusAsync("LUT Preview Toggled");
            }

            @Override
            public void onToggleQuickReview() {
                if (qMenuView != null) {
                    if (qMenuView.isReviewing()) qMenuView.clearQuickReview();
                }
            }

            @Override
            public void onResetParameter() {
                if (cameraAdapter != null) {
                    cameraAdapter.incrementEV(0);
                }
                uiController.updateBottomHud();
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

        // Precompute initial 17x17x17 LUT
        precomputeActiveLut();
        uiController.updateBottomHud();
        
        // Start 10Hz Hardware Poller for Exposure Dials
        if (cameraAdapter instanceof SonyPmcaAdapter) {
            ((SonyPmcaAdapter) cameraAdapter).startHardwarePoller(new Runnable() {
                @Override
                public void run() {
                    uiController.updateBottomHud();
                }
            });
        }
    }

    private void onRecipeUpdated(FujiRecipe r) {
        if (qMenuView != null) {
            qMenuView.setRecipe(r);
            qMenuView.showCarouselBanner(r.filmSim.label);
        }

        // 1. Stage 1: Hardware mapping to Sony ISP (0ms)
        if (cameraAdapter != null) {
            SonyToFujiHardwareMapper.applyRecipeToHardware(
                cameraAdapter.getCameraEx(),
                cameraAdapter.getNormalCamera(),
                r
            );
        }

        // 2. Stage 2: Pre-compute 17x17x17 LUT (~2ms)
        precomputeActiveLut();
        uiController.updateBottomHud();
    }

    private void precomputeActiveLut() {
        FujiRecipe r = appState.getActiveRecipe();
        ColorEngine.precomputeLut17Safe(
            r.computeCcmQ8(),
            r.generateToneCurve('R'),
            r.generateToneCurve('G'),
            r.generateToneCurve('B')
        );
    }

    private void cycleLook(int delta) {
        FujiRecipe recipe = appState.getActiveRecipe();
        int len = FujiRecipe.FilmSim.values().length;
        int next = (recipe.filmSim.ordinal() + delta + len) % len;
        recipe.filmSim = FujiRecipe.FilmSim.values()[next];
        onRecipeUpdated(recipe);
    }

    private boolean isS2PhysicallyHeld = false;
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

        // Strict Hardware Shutter Debouncing (catches 514, 516, 0x202, 513, 515, 0x201)
        if (fujiAction == SonyKeyCodes.FujiAction.FULL_SHUTTER) {
            if (action == KeyEvent.ACTION_DOWN) {
                if (event.getRepeatCount() > 0 || isS2PhysicallyHeld) return true; // Block hardware burst loop
                isS2PhysicallyHeld = true;
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
                if (event.getRepeatCount() > 0 || isS1Held) return true;
                isS1Held = true;
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
        public void onPreviewFrame(byte[] data, android.hardware.Camera camera) {
            if (qMenuView != null && uiController != null && uiController.isLiveViewLutEnabled()) {
                android.hardware.Camera.Size size = camera.getParameters().getPreviewSize();
                if (size != null) {
                    qMenuView.processLiveViewFrame(data, size.width, size.height, appState.getActiveRecipe().grainEffect.ordinal());
                }
            }
            if (cameraAdapter instanceof SonyPmcaAdapter) {
                ((SonyPmcaAdapter) cameraAdapter).addCallbackBuffer(data);
            }
        }
    };

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (cameraAdapter == null) return;
        cameraAdapter.startPreview(holder);

        SonyToFujiHardwareMapper.applyRecipeToHardware(
            cameraAdapter.getCameraEx(),
            cameraAdapter.getNormalCamera(),
            appState.getActiveRecipe()
        );

        if (uiController != null) {
            uiController.updateBottomHud();
        }

        // Register Zero-GC Real-time Preview Callback
        if (cameraAdapter instanceof SonyPmcaAdapter) {
            try {
                android.hardware.Camera.Size previewSize = cameraAdapter.getNormalCamera().getParameters().getPreviewSize();
                if (previewSize != null) {
                    ((SonyPmcaAdapter) cameraAdapter).setRealtimePreviewCallback(realtimePreviewCallback, previewSize.width, previewSize.height);
                }
            } catch (Throwable t) {
                Log.e(TAG, "Failed to initialize realtime preview callback", t);
            }
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (cameraAdapter != null) {
            cameraAdapter.release();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (cameraAdapter != null && cameraPreview != null) {
            cameraAdapter.startPreview(cameraPreview.getHolder());
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (cameraAdapter != null) {
            cameraAdapter.release();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (captureController != null) {
            captureController.shutdown();
        }
        if (cameraAdapter != null) {
            cameraAdapter.release();
            cameraAdapter = null;
        }
    }
}
