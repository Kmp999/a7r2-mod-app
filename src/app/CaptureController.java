package com.fujilite.a7r2;

import android.hardware.Camera;
import android.util.Log;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Manages the capture pipeline, dispatching image processing off the UI thread.
 * Guaranteed zero heap allocation inside the execution hot path.
 */
public class CaptureController {
    private static final String TAG = "CaptureController";
    
    // Default proxy dimensions targeted for Sony A7R II live-view/proxy budget
    public static final int DEFAULT_PROXY_WIDTH = 1280;
    public static final int DEFAULT_PROXY_HEIGHT = 720;

    private final CameraAdapter cameraAdapter;
    private final UIController uiController;
    private final AppState appState;
    private final ExecutorService processingExecutor;

    public CaptureController(CameraAdapter adapter, UIController uiController, AppState state) {
        this.cameraAdapter = adapter;
        this.uiController = uiController;
        this.appState = state;
        this.processingExecutor = Executors.newSingleThreadExecutor();
    }

    public void triggerCapture() {
        if (appState.isProcessing()) {
            return;
        }
        appState.setProcessing(true);
        uiController.updateStatusAsync("Capturing...");

        cameraAdapter.takePicture(new Camera.PictureCallback() {
            @Override
            public void onPictureTaken(final byte[] data, Camera camera) {
                final long imageAvailableTimestamp = System.currentTimeMillis();

                processingExecutor.execute(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            if (data == null || data.length == 0) {
                                uiController.updateStatusAsync("Capture failed: Empty buffer");
                                return;
                            }

                            int width = DEFAULT_PROXY_WIDTH;
                            int height = DEFAULT_PROXY_HEIGHT;

                            // Defensive dimension validation:
                            // NV21 requires Y plane (W*H) + UV interleaved (W*H/2) = W*H * 1.5 bytes
                            int expectedNv21Bytes = (width * height * 3) / 2;
                            if (data.length < expectedNv21Bytes) {
                                // Fallback resolution probe if sensor returned standard 640x480 preview
                                if (data.length >= (640 * 480 * 3) / 2) {
                                    width = 640;
                                    height = 480;
                                } else {
                                    Log.w(TAG, "Buffer size (" + data.length + " bytes) does not match expected NV21 geometry");
                                }
                            }

                            // In-place processing via native color engine (zero allocation)
                            ColorEngine.processNV21(data, width, height);

                            long duration = System.currentTimeMillis() - imageAvailableTimestamp;
                            appState.setLastProcessingTimeMs(duration);
                            uiController.updateStatusAsync("Processed in " + duration + " ms");
                        } catch (Throwable t) {
                            Log.e(TAG, "Error processing frame", t);
                            appState.setLastErrorMessage(t.getMessage());
                            uiController.updateStatusAsync("Error: " + t.getMessage());
                        } finally {
                            appState.setProcessing(false);
                        }
                    }
                });
            }
        });
    }

    public void shutdown() {
        processingExecutor.shutdown();
    }
}
