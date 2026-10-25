package com.fujilite.a7r2;

import android.hardware.Camera;
import android.util.Log;
import android.view.SurfaceHolder;

import java.lang.reflect.Method;
import java.util.List;

/**
 * Camera adapter for Sony PMCA bodies (A7R II).
 *
 * Drives sensor readout via com.sony.scalar.hardware.CameraEx and provides
 * direct hardware controls for exposure (Shutter Speed, Aperture, ISO, EV compensation)
 * and focus / drive mode via CameraEx.ParametersModifier.
 */
public class SonyPmcaAdapter implements CameraAdapter {
    private static final String TAG = "SonyPmcaAdapter";
    private static final String CAMERA_EX_CLASS = "com.sony.scalar.hardware.CameraEx";

    private Object cameraEx;          // com.sony.scalar.hardware.CameraEx (via reflection)
    private Camera normalCamera;      // android.hardware.Camera actually used
    private String activePath = "none";

    // Cached Reflection references for ParametersModifier
    private Method createModifierMethod;
    private Method getShutterSpeedMethod;
    private Method setShutterSpeedMethod;
    private Method getSupportedShutterSpeedMethod;
    private Method getApertureMethod;
    private Method setApertureMethod;
    private Method getSupportedApertureMethod;
    private Method setISOSensitivityMethod;
    private Method getISOSensitivityMethod;
    private Method setDriveModeMethod;

    // Local cached hardware states for Zero-GC HUD updates
    private String cachedShutter = "1/125";
    private String cachedAperture = "F2.8";
    private String cachedIso = "ISO 400";
    private String cachedEv = "+0.0";
    private boolean aelLocked = false;
    private int afModeIndex = 0; // 0: AF-S, 1: AF-C, 2: MF
    private static final String[] AF_MODES = {"AF-S", "AF-C", "MF"};

    public SonyPmcaAdapter() {
        // Opened lazily; never during onCreate.
    }

    public String getActivePath() {
        return activePath;
    }

    public Object getCameraEx() {
        return cameraEx;
    }

    public Camera getNormalCamera() {
        return normalCamera;
    }

    public synchronized boolean ensureOpen() {
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
                    initReflectionCache(cls);
                    setDriveModeSingle();
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

    private void initReflectionCache(Class<?> cls) {
        try {
            createModifierMethod = cls.getMethod("createParametersModifier", Camera.Parameters.class);
            Class<?> modifierCls = Class.forName("com.sony.scalar.hardware.CameraEx$ParametersModifier");

            for (Method m : modifierCls.getMethods()) {
                String name = m.getName();
                Class<?>[] params = m.getParameterTypes();
                if ("getShutterSpeed".equals(name) && params.length == 0) {
                    getShutterSpeedMethod = m;
                } else if ("setShutterSpeed".equals(name) && params.length == 2) {
                    setShutterSpeedMethod = m;
                } else if ("getSupportedShutterSpeed".equals(name)) {
                    getSupportedShutterSpeedMethod = m;
                } else if ("getAperture".equals(name) && params.length == 0) {
                    getApertureMethod = m;
                } else if ("setAperture".equals(name) && params.length == 1) {
                    setApertureMethod = m;
                } else if ("getSupportedAperture".equals(name)) {
                    getSupportedApertureMethod = m;
                } else if ("setISOSensitivity".equals(name) && params.length == 1) {
                    setISOSensitivityMethod = m;
                } else if ("getISOSensitivity".equals(name) && params.length == 0) {
                    getISOSensitivityMethod = m;
                } else if ("setDriveMode".equals(name)) {
                    setDriveModeMethod = m;
                }
            }
            Log.i(TAG, "CameraEx reflection cache initialized successfully");
            registerHardwareListeners(cls);
        } catch (Throwable t) {
            Log.w(TAG, "ParametersModifier reflection cache init failed", t);
        }
    }

    private void registerHardwareListeners(Class<?> cls) {
        if (cameraEx == null) return;
        try {
            // Register ShutterSpeedChangeListener
            Class<?> ssListenerCls = Class.forName("com.sony.scalar.hardware.CameraEx$ShutterSpeedChangeListener");
            Object ssProxy = java.lang.reflect.Proxy.newProxyInstance(
                ssListenerCls.getClassLoader(),
                new Class<?>[]{ssListenerCls},
                new java.lang.reflect.InvocationHandler() {
                    @Override
                    public Object invoke(Object proxy, Method method, Object[] args) {
                        Log.i(TAG, "ShutterSpeedChangeListener triggered by BIONZ X");
                        return null;
                    }
                }
            );
            Method setSsListener = cls.getMethod("setShutterSpeedChangeListener", ssListenerCls);
            setSsListener.invoke(cameraEx, ssProxy);

            // Register ApertureChangeListener
            Class<?> apListenerCls = Class.forName("com.sony.scalar.hardware.CameraEx$ApertureChangeListener");
            Object apProxy = java.lang.reflect.Proxy.newProxyInstance(
                apListenerCls.getClassLoader(),
                new Class<?>[]{apListenerCls},
                new java.lang.reflect.InvocationHandler() {
                    @Override
                    public Object invoke(Object proxy, Method method, Object[] args) {
                        Log.i(TAG, "ApertureChangeListener triggered by BIONZ X");
                        return null;
                    }
                }
            );
            Method setApListener = cls.getMethod("setApertureChangeListener", apListenerCls);
            setApListener.invoke(cameraEx, apProxy);

            Log.i(TAG, "Hardware dial listeners registered successfully via Proxy");
        } catch (Throwable t) {
            Log.w(TAG, "Hardware listener registration unsupported on this body", t);
        }
    }

    private Object getModifier() {
        if (createModifierMethod == null || cameraEx == null || normalCamera == null) return null;
        try {
            return createModifierMethod.invoke(cameraEx, normalCamera.getParameters());
        } catch (Throwable t) {
            return null;
        }
    }

    // ------------------------------------------------------------------------
    // Hardware Poller & Preview Framework
    // ------------------------------------------------------------------------

    private Runnable hardwarePoller;
    private android.os.Handler pollerHandler;
    private boolean isPolling = false;

    private byte[] previewBufferA;
    private byte[] previewBufferB;
    private boolean previewBuffersAllocated = false;

    private void allocatePreviewBuffers(int width, int height) {
        if (previewBuffersAllocated) return;
        int yuvSize = width * height * 3 / 2; // NV21 format
        previewBufferA = new byte[yuvSize];
        previewBufferB = new byte[yuvSize];
        previewBuffersAllocated = true;
        Log.i(TAG, "Allocated double preview buffers: " + yuvSize + " bytes each");
    }

    public void setRealtimePreviewCallback(Camera.PreviewCallback callback, int width, int height) {
        if (!ensureOpen() || normalCamera == null) return;
        try {
            allocatePreviewBuffers(width, height);
            normalCamera.setPreviewCallbackWithBuffer(callback);
            normalCamera.addCallbackBuffer(previewBufferA);
            normalCamera.addCallbackBuffer(previewBufferB);
            Log.i(TAG, "Real-time PreviewCallbackWithBuffer registered");
        } catch (Throwable t) {
            Log.e(TAG, "Failed to setPreviewCallbackWithBuffer", t);
        }
    }

    public void addCallbackBuffer(byte[] buffer) {
        if (normalCamera != null && buffer != null) {
            try {
                normalCamera.addCallbackBuffer(buffer);
            } catch (Throwable ignored) {}
        }
    }

    public void clearPreviewCallback() {
        if (normalCamera != null) {
            try {
                normalCamera.setPreviewCallbackWithBuffer(null);
                normalCamera.setPreviewCallback(null);
            } catch (Throwable ignored) {}
        }
    }

    public void startHardwarePoller(final Runnable onHardwareChange) {
        if (isPolling) return;
        if (pollerHandler == null) {
            pollerHandler = new android.os.Handler(android.os.Looper.getMainLooper());
        }

        isPolling = true;
        hardwarePoller = new Runnable() {
            private String lastShutter = "";
            private String lastAperture = "";
            private String lastIso = "";
            private String lastEv = "";

            @Override
            public void run() {
                if (!isPolling) return;

                String s = getShutterSpeed();
                String a = getAperture();
                String i = getIso();
                String e = getEv();

                if (!s.equals(lastShutter) || !a.equals(lastAperture) || !i.equals(lastIso) || !e.equals(lastEv)) {
                    lastShutter = s;
                    lastAperture = a;
                    lastIso = i;
                    lastEv = e;
                    if (onHardwareChange != null) {
                        onHardwareChange.run();
                    }
                }

                pollerHandler.postDelayed(this, 100); // 10Hz polling
            }
        };
        pollerHandler.post(hardwarePoller);
        Log.i(TAG, "10Hz Hardware Exposure Poller started");
    }

    public void stopHardwarePoller() {
        isPolling = false;
        if (pollerHandler != null && hardwarePoller != null) {
            pollerHandler.removeCallbacks(hardwarePoller);
        }
    }

    private void commitModifier(Object modifier) {
        if (modifier == null || normalCamera == null) return;
        try {
            Method getParams = modifier.getClass().getMethod("getParameters");
            Camera.Parameters params = (Camera.Parameters) getParams.invoke(modifier);
            normalCamera.setParameters(params);
        } catch (Throwable t) {
            Log.e(TAG, "commitModifier failed", t);
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

    /**
     * Interrupts BIONZ X exposure and resets hardware trigger logic.
     */
    public void cancelTakePicture() {
        if (cameraEx != null) {
            try {
                Method cancelM = cameraEx.getClass().getMethod("cancelTakePicture");
                cancelM.invoke(cameraEx);
            } catch (Throwable t) {
                Log.d(TAG, "cancelTakePicture not supported on this CameraEx build");
            }
        }
        if (normalCamera != null) {
            try {
                normalCamera.cancelAutoFocus();
            } catch (Throwable ignored) {}
        }
    }

    public void autoFocus() {
        if (normalCamera != null) {
            try {
                normalCamera.autoFocus(null);
            } catch (Throwable t) {
                Log.d(TAG, "autoFocus failed", t);
            }
        }
    }

    public void cancelAutoFocus() {
        if (normalCamera != null) {
            try {
                normalCamera.cancelAutoFocus();
            } catch (Throwable ignored) {}
        }
    }

    /**
     * Enforces Single Shot drive mode to permanently defeat the Infinite Capture Loop.
     */
    public void setDriveModeSingle() {
        try {
            Object modifier = getModifier();
            if (modifier != null && setDriveModeMethod != null) {
                Class<?>[] pTypes = setDriveModeMethod.getParameterTypes();
                if (pTypes.length > 0 && pTypes[0] == int.class) {
                    setDriveModeMethod.invoke(modifier, 0); // DRIVE_MODE_SINGLE = 0
                } else if (pTypes.length > 0 && pTypes[0] == String.class) {
                    setDriveModeMethod.invoke(modifier, "single");
                }
                commitModifier(modifier);
                Log.i(TAG, "Drive mode enforced: Single-Shot");
            }
        } catch (Throwable t) {
            Log.w(TAG, "setDriveModeSingle failed", t);
        }
    }

    // ------------------------------------------------------------------------
    // Hardware Exposure Control (Shutter Speed, Aperture, ISO, EV)
    // ------------------------------------------------------------------------

    public void incrementShutterSpeed(int step) {
        try {
            Object modifier = getModifier();
            if (modifier != null && getSupportedShutterSpeedMethod != null && setShutterSpeedMethod != null) {
                List<?> supported = (List<?>) getSupportedShutterSpeedMethod.invoke(modifier);
                Object cur = getShutterSpeedMethod.invoke(modifier);
                if (supported != null && !supported.isEmpty() && cur != null) {
                    int idx = supported.indexOf(cur);
                    if (idx >= 0) {
                        int next = Math.max(0, Math.min(supported.size() - 1, idx + step));
                        Object nextVal = supported.get(next);
                        Class<?> pairCls = nextVal.getClass();
                        Object first = pairCls.getField("first").get(nextVal);
                        Object second = pairCls.getField("second").get(nextVal);
                        if (first instanceof Integer && second instanceof Integer) {
                            setShutterSpeedMethod.invoke(modifier, ((Integer) first).intValue(), ((Integer) second).intValue());
                            commitModifier(modifier);
                            cachedShutter = formatShutterSpeed(((Integer) first).intValue(), ((Integer) second).intValue());
                            return;
                        }
                    }
                }
            }
        } catch (Throwable ignored) {}

        // Fallback simulation
        cachedShutter = (step > 0) ? "1/250" : "1/60";
    }

    public String getShutterSpeed() {
        try {
            Object modifier = getModifier();
            if (modifier != null && getShutterSpeedMethod != null) {
                Object cur = getShutterSpeedMethod.invoke(modifier);
                if (cur != null) {
                    Class<?> pairCls = cur.getClass();
                    Object first = pairCls.getField("first").get(cur);
                    Object second = pairCls.getField("second").get(cur);
                    if (first instanceof Integer && second instanceof Integer) {
                        cachedShutter = formatShutterSpeed(((Integer) first).intValue(), ((Integer) second).intValue());
                    }
                }
            }
        } catch (Throwable ignored) {}
        return cachedShutter;
    }

    private String formatShutterSpeed(int num, int den) {
        if (num <= 0 || den <= 0) return "AUTO";
        if (num == 1) return "1/" + den;
        if (den == 1) return num + "\"";
        float sec = (float) num / (float) den;
        return String.format("%.1f\"", sec);
    }

    public void incrementAperture(int step) {
        try {
            Object modifier = getModifier();
            if (modifier != null && getSupportedApertureMethod != null && setApertureMethod != null) {
                List<?> supported = (List<?>) getSupportedApertureMethod.invoke(modifier);
                Object cur = getApertureMethod.invoke(modifier);
                if (supported != null && !supported.isEmpty() && cur != null) {
                    int idx = supported.indexOf(cur);
                    if (idx >= 0) {
                        int next = Math.max(0, Math.min(supported.size() - 1, idx + step));
                        Object nextVal = supported.get(next);
                        if (nextVal instanceof Integer) {
                            setApertureMethod.invoke(modifier, ((Integer) nextVal).intValue());
                            commitModifier(modifier);
                            cachedAperture = String.format("F%.1f", ((Integer) nextVal).intValue() / 100.0f);
                            return;
                        }
                    }
                }
            }
        } catch (Throwable ignored) {}

        cachedAperture = (step > 0) ? "F4.0" : "F2.0";
    }

    public String getAperture() {
        try {
            Object modifier = getModifier();
            if (modifier != null && getApertureMethod != null) {
                Object cur = getApertureMethod.invoke(modifier);
                if (cur instanceof Integer) {
                    cachedAperture = String.format("F%.1f", ((Integer) cur).intValue() / 100.0f);
                }
            }
        } catch (Throwable ignored) {}
        return cachedAperture;
    }

    public void setISOSensitivity(int iso) {
        try {
            Object modifier = getModifier();
            if (modifier != null && setISOSensitivityMethod != null) {
                setISOSensitivityMethod.invoke(modifier, iso);
                commitModifier(modifier);
                cachedIso = (iso == 0) ? "ISO AUTO" : "ISO " + iso;
            }
        } catch (Throwable ignored) {}
    }

    public String getIso() {
        try {
            Object modifier = getModifier();
            if (modifier != null && getISOSensitivityMethod != null) {
                Object cur = getISOSensitivityMethod.invoke(modifier);
                if (cur instanceof Integer) {
                    int iso = ((Integer) cur).intValue();
                    cachedIso = (iso == 0) ? "ISO AUTO" : "ISO " + iso;
                }
            }
        } catch (Throwable ignored) {}
        return cachedIso;
    }

    public void incrementEV(int step) {
        if (normalCamera == null) return;
        try {
            Camera.Parameters params = normalCamera.getParameters();
            int cur = params.getExposureCompensation();
            int min = params.getMinExposureCompensation();
            int max = params.getMaxExposureCompensation();
            int next = Math.max(min, Math.min(max, cur + step));
            params.setExposureCompensation(next);
            normalCamera.setParameters(params);
            float evVal = next * params.getExposureCompensationStep();
            cachedEv = (evVal >= 0 ? "+" : "") + String.format("%.1f", evVal);
        } catch (Throwable ignored) {}
    }

    public String getEv() {
        return cachedEv;
    }

    public void setAutoExposureLock(boolean locked) {
        this.aelLocked = locked;
        if (normalCamera == null) return;
        try {
            Camera.Parameters params = normalCamera.getParameters();
            if (params.isAutoExposureLockSupported()) {
                params.setAutoExposureLock(locked);
                normalCamera.setParameters(params);
            }
        } catch (Throwable ignored) {}
    }

    public boolean isAutoExposureLock() {
        return aelLocked;
    }

    public String toggleAfMode() {
        afModeIndex = (afModeIndex + 1) % AF_MODES.length;
        String mode = AF_MODES[afModeIndex];
        if (normalCamera != null) {
            try {
                Camera.Parameters params = normalCamera.getParameters();
                if ("MF".equals(mode)) {
                    params.setFocusMode(Camera.Parameters.FOCUS_MODE_FIXED);
                } else if ("AF-C".equals(mode)) {
                    params.setFocusMode(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE);
                } else {
                    params.setFocusMode(Camera.Parameters.FOCUS_MODE_AUTO);
                }
                normalCamera.setParameters(params);
            } catch (Throwable ignored) {}
        }
        return mode;
    }

    public String getAfMode() {
        return AF_MODES[afModeIndex];
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
        return false;
    }
}
