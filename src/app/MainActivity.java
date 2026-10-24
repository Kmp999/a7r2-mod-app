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

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    
    private SurfaceView cameraPreview;
    private TextView statusText;
    private Button captureBtn;

    private AppState appState;
    private UIController uiController;
    private SonyPmcaAdapter cameraAdapter;
    private CaptureController captureController;

    // Load C native engine
    static {
        System.loadLibrary("color_engine_jni");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Core component orchestration
        appState = new AppState();
        cameraAdapter = new SonyPmcaAdapter();

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

        // Initialize UI controller & capture controller
        uiController = new UIController(statusText);
        captureController = new CaptureController(cameraAdapter, uiController, appState);

        // Initialize Native Color Engine with Provia preset
        ColorEngine.initEngine(appState.getCurrentPresetId(), ColorEngine.MODE_SAFE_FASTEST);

        // Bind SurfaceHolder for camera preview
        cameraPreview.getHolder().addCallback(this);

        // Direct capture button wiring
        captureBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                captureController.triggerCapture();
            }
        });
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
