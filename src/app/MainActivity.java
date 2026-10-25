package com.fujilite.a7r2;

import android.app.Activity;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Button;
import android.widget.TextView;
import android.widget.LinearLayout;
import android.view.View;
import android.graphics.Color;
import android.util.Log;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    private static final String TAG = "FujiLiteMain";

    private SurfaceView cameraPreview;
    private TextView statusText;
    private Button captureBtn;

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
        // Minimalistic UI without XML inflation for Dalvik efficiency
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setBackgroundColor(Color.BLACK);

        cameraPreview = new SurfaceView(this);
        LinearLayout.LayoutParams previewParams = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0, 1.0f);
        layout.addView(cameraPreview, previewParams);

        LinearLayout controlPanel = new LinearLayout(this);
        controlPanel.setOrientation(LinearLayout.HORIZONTAL);

        captureBtn = new Button(this);
        captureBtn.setText("CAPTURE");

        statusText = new TextView(this);
        statusText.setText("Mode: Proxy 720p | Preset: Provia");
        statusText.setTextColor(Color.WHITE);
        statusText.setPadding(20, 20, 20, 20);

        controlPanel.addView(captureBtn);
        controlPanel.addView(statusText);
        layout.addView(controlPanel);

        setContentView(layout);

        // Direct capture button wiring
        captureBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (captureController != null) {
                    captureController.triggerCapture();
                }
            }
        });
    }

    private void initPipeline() {
        // Core component orchestration
        appState = new AppState();
        cameraAdapter = new SonyPmcaAdapter();

        // Initialize UI controller & capture controller
        uiController = new UIController(statusText);
        captureController = new CaptureController(cameraAdapter, uiController, appState);

        // Initialize Native Color Engine with Provia preset (non-fatal if the
        // native library is missing or incompatible).
        ColorEngine.initEngineSafe(appState.getCurrentPresetId(), ColorEngine.MODE_SAFE_FASTEST);
        if (!ColorEngine.isNativeAvailable() && statusText != null) {
            statusText.setText("Native engine unavailable - passthrough mode");
        }

        // Bind SurfaceHolder for camera preview
        cameraPreview.getHolder().addCallback(this);
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
        if (cameraAdapter != null) {
            cameraAdapter.startPreview(holder);
        }
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
        if (cameraAdapter == null) {
            cameraAdapter = new SonyPmcaAdapter();
            if (captureController != null) {
                captureController = new CaptureController(cameraAdapter, uiController, appState);
            }
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
