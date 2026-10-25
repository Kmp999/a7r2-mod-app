package com.fujilite.a7r2;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
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

    private SurfaceView cameraPreview;
    private TextView statusText;
    private Button captureBtn;
    private Button presetBtn;
    private ImageView resultView;
    private TextView resultInfo;
    private Bitmap lastResult;

    private final Handler ui = new Handler(Looper.getMainLooper());

    private AppState appState;
    private UIController uiController;
    private SonyPmcaAdapter cameraAdapter;
    private CaptureController captureController;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // The whole startup is guarded: on the A7R II any uncaught exception or
        // native fault here makes the camera vibrate once and bounce back to the
        // application menu. We would rather stay open and report the problem.
        try {
            buildUi();
            initPipeline();
        } catch (Throwable t) {
            Log.e(TAG, "Startup failed; continuing in degraded mode", t);
            showFatalSafe(t);
        }
    }

    private void buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);

        cameraPreview = new SurfaceView(this);
        try {
            // Sony live view requires a push-buffer surface.
            cameraPreview.getHolder().setType(SurfaceHolder.SURFACE_TYPE_PUSH_BUFFERS);
        } catch (Throwable ignored) {
        }
        layout.addView(cameraPreview, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

        LinearLayout controlPanel = new LinearLayout(this);
        controlPanel.setOrientation(LinearLayout.HORIZONTAL);

        presetBtn = new Button(this);
        presetBtn.setText("LOOK: Provia");
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

        root.addView(layout, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        // Full-screen result overlay (tap to dismiss and return to live view).
        resultView = new ImageView(this);
        resultView.setBackgroundColor(Color.BLACK);
        resultView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        resultView.setVisibility(View.GONE);
        root.addView(resultView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        resultInfo = new TextView(this);
        resultInfo.setTextColor(Color.WHITE);
        resultInfo.setBackgroundColor(0x99000000);
        resultInfo.setPadding(20, 20, 20, 20);
        resultInfo.setVisibility(View.GONE);
        FrameLayout.LayoutParams infoLp = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        infoLp.gravity = Gravity.BOTTOM;
        root.addView(resultInfo, infoLp);

        setContentView(root);

        presetBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (appState == null) return;
                appState.cycleNextPreset();
                String name = appState.getCurrentPresetName();
                presetBtn.setText("LOOK: " + name);
                statusText.setText("Look: " + name);
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
        captureController = new CaptureController(cameraAdapter, uiController, appState);
        presetBtn.setText("LOOK: " + appState.getCurrentPresetName());

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

        // Native engine is loaded lazily on first capture (never during onCreate).
        cameraPreview.getHolder().addCallback(this);
    }

    private void showResult(Bitmap graded, String info) {
        if (lastResult != null && lastResult != graded) {
            lastResult.recycle();
        }
        lastResult = graded;
        resultView.setImageBitmap(graded);
        resultView.setVisibility(View.VISIBLE);
        resultInfo.setText(info + "\n(tap image to return to live view)");
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
            // Never let error reporting itself crash the activity.
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (cameraAdapter == null) return;
        cameraAdapter.startPreview(holder);
        final String path = cameraAdapter.getActivePath();
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
        // Handled directly by Sony hardware layer
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
        // Reopen and restart preview when returning from pause. On first launch the
        // surface is not ready yet; surfaceCreated() will start it. Both paths are
        // guarded inside the adapter.
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
