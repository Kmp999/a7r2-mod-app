package com.fujilite.a7r2;

import android.hardware.Camera;
import android.view.SurfaceHolder;

/**
 * Agnostic interface for camera operations.
 * Isolates the rest of the app from standard vs Sony-specific APIs.
 */
public interface CameraAdapter {
    void startPreview(Camera.PreviewCallback callback);
    void startPreview(SurfaceHolder holder);
    void takePicture(Camera.PictureCallback jpegCallback);
    void release();
    // TODO(SONY_SDK): Verify if we can set picture profiles natively here.
    boolean supportsInternalColorPipeline();
}
