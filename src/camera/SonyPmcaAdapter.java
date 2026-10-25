package com.fujilite.a7r2;

import android.hardware.Camera;
import android.util.Log;

/**
 * Adapter that safely attempts to use com.sony.scalar extensions,
 * falling back to standard Android Camera API.
 */
public class SonyPmcaAdapter implements CameraAdapter {
    private static final String TAG = "SonyPmcaAdapter";
    private Camera mCamera;

    public SonyPmcaAdapter() {
        // Do NOT open the camera in the constructor. On the A7R II the Sony
        // firmware owns the sensor and a synchronous Camera.open() during
        // onCreate can abort natively (uncaught by catch(Exception)) and bounce
        // the app back to the menu. Open lazily and defensively instead.
    }

    /** Opens the hardware camera on demand; never throws. */
    private boolean ensureOpen() {
        if (mCamera != null) return true;
        try {
            mCamera = Camera.open();
        } catch (Throwable t) {
            Log.e(TAG, "Failed to open camera", t);
            mCamera = null;
        }
        return mCamera != null;
    }

    @Override
    public void startPreview(Camera.PreviewCallback callback) {
        if (!ensureOpen()) return;
        try {
            mCamera.setPreviewCallback(callback);
            mCamera.startPreview();
        } catch (Throwable t) {
            Log.e(TAG, "Failed to start preview callback", t);
        }
    }

    @Override
    public void startPreview(android.view.SurfaceHolder holder) {
        if (!ensureOpen()) return;
        try {
            mCamera.setPreviewDisplay(holder);
            mCamera.startPreview();
        } catch (Throwable t) {
            Log.e(TAG, "Failed to start preview on SurfaceHolder", t);
        }
    }

    @Override
    public void takePicture(Camera.PictureCallback jpegCallback) {
        if (!ensureOpen()) return;
        try {
            // Null for Shutter and RAW/Postview callbacks to maximize speed
            mCamera.takePicture(null, null, jpegCallback);
        } catch (Throwable t) {
            Log.e(TAG, "takePicture failed", t);
        }
    }

    @Override
    public void release() {
        if (mCamera != null) {
            try {
                mCamera.setPreviewCallback(null);
                mCamera.release();
            } catch (Throwable t) {
                Log.e(TAG, "Failed to release camera", t);
            }
            mCamera = null;
        }
    }

    @Override
    public boolean supportsInternalColorPipeline() {
        // TODO(SONY_SDK): Return true if CameraEx.ParametersModifier is detected and usable.
        return false;
    }
}
