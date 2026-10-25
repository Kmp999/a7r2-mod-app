package com.fujilite.a7r2;

import android.hardware.Camera;
import android.util.Log;
import android.view.SurfaceHolder;

import java.lang.reflect.Method;

/**
 * Camera adapter for Sony PMCA bodies (A7R II).
 *
 * The standard android.hardware.Camera.open() does NOT drive the A7-series
 * imaging sensor - it yields a black preview and no capture. The proven path
 * (see ma1co PMCADemo) is Sony's com.sony.scalar.hardware.CameraEx:
 *
 *     CameraEx cam = CameraEx.open(0, null);
 *     Camera normal = cam.getNormalCamera();   // a real android.hardware.Camera
 *     normal.setPreviewDisplay(holder); normal.startPreview();
 *     normal.takePicture(null, null, jpegCallback);
 *
 * CameraEx is not in android.jar, and this project builds with raw javac/dx (no
 * compileOnly stubs), so we reach it through reflection and fall back to the
 * standard Camera API when the Sony class is unavailable (e.g. host testing).
 */
public class SonyPmcaAdapter implements CameraAdapter {
    private static final String TAG = "SonyPmcaAdapter";
    private static final String CAMERA_EX_CLASS = "com.sony.scalar.hardware.CameraEx";

    private Object cameraEx;          // com.sony.scalar.hardware.CameraEx (via reflection)
    private Camera normalCamera;      // android.hardware.Camera actually used
    private String activePath = "none";

    public SonyPmcaAdapter() {
        // Opened lazily; never during onCreate.
    }

    /** Returns which camera backend is live: "CameraEx", "Camera", or "none". */
    public String getActivePath() {
        return activePath;
    }

    private boolean ensureOpen() {
        if (normalCamera != null) return true;

        // 1) Preferred: Sony CameraEx (the only backend that drives the A7R II sensor).
        try {
            Class<?> cls = Class.forName(CAMERA_EX_CLASS);
            Method openM = null;
            for (Method m : cls.getMethods()) {
                if ("open".equals(m.getName()) && m.getParameterTypes().length == 2) {
                    openM = m;
                    break;
                }
            }
            if (openM != null) {
                cameraEx = openM.invoke(null, 0, null);
                Object nc = cls.getMethod("getNormalCamera").invoke(cameraEx);
                if (nc instanceof Camera) {
                    normalCamera = (Camera) nc;
                    activePath = "CameraEx";
                    return true;
                }
                Log.e(TAG, "getNormalCamera() returned " + (nc == null ? "null" : nc.getClass().getName()));
                releaseCameraEx();
            }
        } catch (ClassNotFoundException e) {
            Log.i(TAG, "CameraEx not present; falling back to standard Camera");
        } catch (Throwable t) {
            Log.e(TAG, "CameraEx open failed; falling back to standard Camera", t);
            releaseCameraEx();
        }

        // 2) Fallback: standard Camera (works on host/mock, limited on A7R II).
        try {
            normalCamera = Camera.open();
            activePath = normalCamera != null ? "Camera" : "none";
            return normalCamera != null;
        } catch (Throwable t) {
            Log.e(TAG, "Standard Camera.open() failed", t);
            normalCamera = null;
            activePath = "none";
            return false;
        }
    }

    @Override
    public void startPreview(Camera.PreviewCallback callback) {
        if (!ensureOpen()) return;
        try {
            normalCamera.setPreviewCallback(callback);
            normalCamera.startPreview();
        } catch (Throwable t) {
            Log.e(TAG, "startPreview(callback) failed", t);
        }
    }

    @Override
    public void startPreview(SurfaceHolder holder) {
        if (!ensureOpen()) return;
        try {
            normalCamera.setPreviewDisplay(holder);
            normalCamera.startPreview();
        } catch (Throwable t) {
            Log.e(TAG, "startPreview(holder) failed", t);
        }
    }

    @Override
    public void takePicture(Camera.PictureCallback jpegCallback) {
        if (!ensureOpen()) return;
        try {
            // Null shutter/raw callbacks for speed; jpeg callback delivers full-res bytes.
            normalCamera.takePicture(null, null, jpegCallback);
        } catch (Throwable t) {
            Log.e(TAG, "takePicture failed", t);
        }
    }

    @Override
    public void resumePreview() {
        if (normalCamera == null) return;
        try {
            normalCamera.startPreview();
        } catch (Throwable t) {
            Log.e(TAG, "resumePreview failed", t);
        }
    }

    private void releaseCameraEx() {
        if (cameraEx != null) {
            try {
                cameraEx.getClass().getMethod("release").invoke(cameraEx);
            } catch (Throwable t) {
                Log.e(TAG, "CameraEx.release failed", t);
            }
            cameraEx = null;
        }
    }

    @Override
    public void release() {
        if (normalCamera != null) {
            try {
                normalCamera.setPreviewCallback(null);
                normalCamera.release();
            } catch (Throwable t) {
                Log.e(TAG, "Camera release failed", t);
            }
            normalCamera = null;
        }
        releaseCameraEx();
        activePath = "none";
    }

    @Override
    public boolean supportsInternalColorPipeline() {
        // TODO(SONY_SDK): Detect CameraEx.ParametersModifier for picture-profile control.
        return false;
    }
}
