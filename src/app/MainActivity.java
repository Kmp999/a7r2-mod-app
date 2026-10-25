package com.fujilite.a7r2;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    private static final String TAG = "FujiLiteMain";

    private FrameLayout rootLayout;
    private SurfaceView cameraPreview;
    private TextView statusText;
    private Button captureBtn;
    private Button presetBtn;
    private ImageView resultView;
    private TextView resultInfo;
    private Bitmap lastResult;

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
            showFatalSafe(t);
        }
    }

    private void buildUi() {
        rootLayout = new FrameLayout(this);
        rootLayout.setBackgroundColor(Color.BLACK);

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);

        cameraPreview = new SurfaceView(this);
        try {
            cameraPreview.getHolder().setType(SurfaceHolder.SURFACE_TYPE_PUSH_BUFFERS);
        } catch (Throwable ignored) {
        }
        layout.addView(cameraPreview, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

        LinearLayout controlPanel = new LinearLayout(this);
        controlPanel.setOrientation(LinearLayout.HORIZONTAL);

        presetBtn = new Button(this);
        presetBtn.setText("Q-MENU");
        captureBtn = new Button(this);
        captureBtn.setText("CAPTURE");
        Button demoBtn = new Button(this);
        demoBtn.setText("DEMO");

        statusText = new TextView(this);
        statusText.setText("Fuji Lite ready");
        statusText.setTextColor(Color.WHITE);
        statusText.setPadding(16, 16, 16, 16);

        controlPanel.addView(presetBtn);
        controlPanel.addView(demoBtn);
        controlPanel.addView(captureBtn);
        controlPanel.addView(statusText);
        layout.addView(controlPanel);

        rootLayout.addView(layout, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        // Result preview overlay
        resultView = new ImageView(this);
        resultView.setBackgroundColor(Color.BLACK);
        resultView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        resultView.setVisibility(View.GONE);
        rootLayout.addView(resultView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        resultInfo = new TextView(this);
        resultInfo.setTextColor(Color.WHITE);
        resultInfo.setBackgroundColor(0x99000000);
        resultInfo.setPadding(20, 20, 20, 20);
        resultInfo.setVisibility(View.GONE);
        FrameLayout.LayoutParams infoLp = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        infoLp.gravity = Gravity.BOTTOM;
        rootLayout.addView(resultInfo, infoLp);

        setContentView(rootLayout);

        presetBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                toggleQMenu();
            }
        });

        demoBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (captureController != null) captureController.runDemo();
            }
        });

        captureBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (captureController != null) captureController.triggerCapture();
            }
        });

        resultView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                hideResult();
            }
        });
    }

    private void initPipeline() {
        appState = new AppState();
        cameraAdapter = new SonyPmcaAdapter();
        uiController = new UIController(statusText);
        uiController.setCameraAdapter(cameraAdapter);
        uiController.setControllerCallback(new UIController.ControllerCallback() {
            @Override
            public void onTriggerCapture() {
                if (captureController != null) captureController.triggerCapture();
            }

            @Override
            public void onToggleQMenu() {
                if (qMenuView != null && qMenuView.getVisibility() == View.VISIBLE) {
                    hideQMenu();
                } else {
                    showQMenu();
                }
            }

            @Override
            public boolean isQMenuOpen() {
                return qMenuView != null && qMenuView.getVisibility() == View.VISIBLE;
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
                if (lastResult != null && resultView.getVisibility() == View.VISIBLE) {
                    hideResult();
                } else if (lastResult != null) {
                    resultView.setVisibility(View.VISIBLE);
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
        captureController = new CaptureController(cameraAdapter, uiController, appState);

        // Load saved recipe slot 0 (C1)
        FujiRecipe recipe = appState.getActiveRecipe();
        recipe.loadFromSlot(this, 0);

        presetBtn.setText("Q: " + recipe.filmSim.label);

        // Build Q-Menu View
        qMenuView = new FujiQMenuView(this, recipe, new FujiQMenuView.OnRecipeChangeListener() {
            @Override
            public void onRecipeChanged(FujiRecipe r) {
                onRecipeUpdated(r);
            }

            @Override
            public void onDismissRequest() {
                hideQMenu();
            }
        });
        qMenuView.setVisibility(View.GONE);
        rootLayout.addView(qMenuView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        captureController.setResultListener(new CaptureController.ResultListener() {
            @Override
            public void onResult(final Bitmap graded, final String info) {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        showResult(graded, info);
                    }
                });
            }
        });

        cameraPreview.getHolder().addCallback(this);

        // Initial precomputation of 17x17x17 LUT
        precomputeActiveLut();
    }

    private void onRecipeUpdated(FujiRecipe r) {
        presetBtn.setText("Q: " + r.filmSim.label);
        if (statusText != null) {
            statusText.setText(r.filmSim.label + " | DR: " + r.dynamicRange.label);
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

    private void toggleQMenu() {
        if (qMenuView == null) return;
        if (qMenuView.getVisibility() == View.VISIBLE) {
            hideQMenu();
        } else {
            showQMenu();
        }
    }

    private void showQMenu() {
        if (qMenuView == null) return;
        qMenuView.setRecipe(appState.getActiveRecipe());
        qMenuView.setVisibility(View.VISIBLE);
        qMenuView.requestFocus();
        appState.setQMenuVisible(true);
    }

    private void hideQMenu() {
        if (qMenuView == null) return;
        qMenuView.setVisibility(View.GONE);
        appState.setQMenuVisible(false);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        // If result preview is visible, any key dismisses it
        if (resultView.getVisibility() == View.VISIBLE) {
            hideResult();
            return true;
        }

        // Anti-repeat check for shutter to prevent infinite capture loop
        if (event.getRepeatCount() > 0) {
            if (keyCode == SonyKeyCodes.SONY_KEY_SHUTTER || keyCode == SonyKeyCodes.SONY_KEY_SHUTTER_HALF) {
                return true;
            }
        }

        // Delegate to UIController's dual-layer resolver first
        if (uiController != null && uiController.handleKeyDown(keyCode, event)) {
            return true;
        }

        // If Q-Menu is open, delegate all hardware keys to Q-Menu
        if (qMenuView != null && qMenuView.getVisibility() == View.VISIBLE) {
            return qMenuView.onKeyDown(keyCode, event);
        }

        // Fallback when Q-Menu is hidden:
        switch (keyCode) {
            // Open Q-Menu: Trash / C3 / Fn / Menu
            case SonyKeyCodes.SONY_KEY_C3_TRASH:
            case SonyKeyCodes.ANDROID_DEL:
            case SonyKeyCodes.ANDROID_CLEAR:
            case SonyKeyCodes.SONY_KEY_FN:
            case KeyEvent.KEYCODE_MENU:
                showQMenu();
                return true;

            // Trigger Capture: Full Shutter press or Camera key
            case SonyKeyCodes.SONY_KEY_SHUTTER:
            case SonyKeyCodes.ANDROID_CAMERA:
                if (captureController != null) {
                    captureController.triggerCapture();
                }
                return true;

            // Dials quick cycle film simulations
            case SonyKeyCodes.SONY_KEY_DIAL2_CW:
            case SonyKeyCodes.SONY_KEY_DIAL3_CW:
            case KeyEvent.KEYCODE_PAGE_UP:
                cycleLook(1);
                return true;

            case SonyKeyCodes.SONY_KEY_DIAL2_CCW:
            case SonyKeyCodes.SONY_KEY_DIAL3_CCW:
            case KeyEvent.KEYCODE_PAGE_DOWN:
                cycleLook(-1);
                return true;
        }

        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (uiController != null && uiController.handleKeyUp(keyCode, event)) {
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    private void cycleLook(int delta) {
        FujiRecipe recipe = appState.getActiveRecipe();
        int len = FujiRecipe.FilmSim.values().length;
        int next = (recipe.filmSim.ordinal() + delta + len) % len;
        recipe.filmSim = FujiRecipe.FilmSim.values()[next];
        onRecipeUpdated(recipe);
    }

    private void showResult(Bitmap graded, String info) {
        if (lastResult != null && lastResult != graded) {
            lastResult.recycle();
        }
        lastResult = graded;
        resultView.setImageBitmap(graded);
        resultView.setVisibility(View.VISIBLE);
        resultInfo.setText(info + "\n(Press Shutter or Trash to return)");
        resultInfo.setVisibility(View.VISIBLE);
    }

    private void hideResult() {
        resultView.setVisibility(View.GONE);
        resultInfo.setVisibility(View.GONE);
        resultView.setImageDrawable(null);
        if (lastResult != null) {
            lastResult.recycle();
            lastResult = null;
        }
    }

    private void showFatalSafe(Throwable t) {
        try {
            if (statusText == null) {
                statusText = new TextView(this);
                statusText.setTextColor(Color.WHITE);
                setContentView(statusText);
            }
            statusText.setText("Startup error: " + t.getClass().getSimpleName());
        } catch (Throwable ignored) {
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (cameraAdapter == null) return;
        cameraAdapter.startPreview(holder);
        final String path = cameraAdapter.getActivePath();

        // Apply initial hardware recipe to camera
        SonyToFujiHardwareMapper.applyRecipeToHardware(
            cameraAdapter.getCameraEx(),
            cameraAdapter.getNormalCamera(),
            appState.getActiveRecipe()
        );

        ui.post(new Runnable() {
            @Override
            public void run() {
                if (statusText != null) {
                    statusText.setText("none".equals(path)
                        ? "No camera - use DEMO to preview looks"
                        : "Live (" + path + ") - " + appState.getCurrentPresetName());
                }
            }
        });
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
        if (lastResult != null) {
            lastResult.recycle();
            lastResult = null;
        }
    }
}
