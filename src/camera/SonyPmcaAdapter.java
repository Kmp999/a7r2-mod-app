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
        try {
            mCamera = Camera.open();
            // TODO(SONY_SDK): Use reflection to see if com.sony.scalar.hardware.CameraEx is available.
        } catch (Exception e) {
            Log.e(TAG, "Failed to open camera", e);
        }
    }

    @Override
    public void startPreview(Camera.PreviewCallback callback) {
        if (mCamera != null) {
            mCamera.setPreviewCallback(callback);
            mCamera.startPreview();
        }
    }

    @Override
    public void startPreview(android.view.SurfaceHolder holder) {
        if (mCamera != null) {
            try {
                mCamera.setPreviewDisplay(holder);
                mCamera.startPreview();
            } catch (Exception e) {
                Log.e(TAG, "Failed to start preview on SurfaceHolder", e);
            }
        }
    }

    @Override
    public void takePicture(Camera.PictureCallback jpegCallback) {
        if (mCamera != null) {
            // Null for Shutter and RAW/Postview callbacks to maximize speed
            mCamera.takePicture(null, null, jpegCallback);
        }
    }

    @Override
    public void release() {
        if (mCamera != null) {
            mCamera.setPreviewCallback(null);
            mCamera.release();
            mCamera = null;
        }
    }

    @Override
    public boolean supportsInternalColorPipeline() {
        // TODO(SONY_SDK): Return true if CameraEx.ParametersModifier is detected and usable.
        return false;
    }
}
