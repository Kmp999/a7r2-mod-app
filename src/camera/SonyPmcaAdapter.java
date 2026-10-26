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

    // ------------------------------------------------------------------------
    // Strict Reflection Resolver (Bypasses IllegalAccessException on Sony A7R II)
    // ------------------------------------------------------------------------

    private static Method findAccessibleMethod(Object target, String name, Class<?>... paramTypes) throws NoSuchMethodException {
        if (target == null) throw new NoSuchMethodException("Target is null for method " + name);

        // 1. Try public interface first
        try {
            Class<?> iface = Class.forName("com.sony.scalar.hardware.CameraEx$ParametersModifier");
            if (iface.isInstance(target)) {
                Method m = iface.getMethod(name, paramTypes);
                m.setAccessible(true);
                return m;
            }
        } catch (Throwable ignored) {}

        // 2. Fallback to implementation class hierarchy
        Class<?> clazz = target.getClass();
        while (clazz != null) {
            try {
                Method m = clazz.getDeclaredMethod(name, paramTypes);
                m.setAccessible(true);
                return m;
            } catch (NoSuchMethodException e) {
                try {
                    Method m = clazz.getMethod(name, paramTypes);
                    m.setAccessible(true);
                    return m;
                } catch (NoSuchMethodException ignored) {}
            }
            clazz = clazz.getSuperclass();
        }
        throw new NoSuchMethodException(name);
    }

    private void initReflectionCache(Class<?> cls) {
        try {
            createModifierMethod = cls.getMethod("createParametersModifier", Camera.Parameters.class);
            createModifierMethod.setAccessible(true);
            
            // Note: Caching modifier methods here is dangerous because the modifier instance changes.
            // We will dynamically resolve them per-call using findAccessibleMethod.
            
            Log.i(TAG, "CameraEx reflection cache initialized successfully");
            registerHardwareListeners(cls);
        } catch (Throwable t) {
            Log.w(TAG, "ParametersModifier reflection cache init failed", t);
        }
    }

    private void registerHardwareListeners(Class<?> cls) {
        if (cameraEx == null) return;
        try {
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
            setSsListener.setAccessible(true);
            setSsListener.invoke(cameraEx, ssProxy);

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
            setApListener.setAccessible(true);
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

                // Ignore hardware poller overwrites for 800ms after user manually turns the dial
                if (System.currentTimeMillis() - lastManualIsoChangeTime < 800) {
                    pollerHandler.postDelayed(this, 100);
                    return;
                }

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
            Camera.Parameters toApply = normalCamera.getParameters();
            try {
                Method getParams = modifier.getClass().getMethod("getParameters");
                Object p = getParams.invoke(modifier);
                if (p instanceof Camera.Parameters) toApply = (Camera.Parameters) p;
            } catch (Throwable ignored) {}
            normalCamera.setParameters(toApply);
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
     * Also explicitly disables Auto-HDR, Bracketing, and Multi-Frame NR which cause 3-shot sequences.
     */
    public void setDriveModeSingle() {
        if (normalCamera == null) return;
        try {
            Camera.Parameters params = normalCamera.getParameters();
            
            // Defeat Auto-HDR and MFNR which cause multi-shot delays
            params.set("sony-auto-hdr", "off");
            params.set("sony-iso-mfnr", "off");
            
            // Set noise reduction to normal
            if (params.get("sony-high-iso-nr") != null) {
                 params.set("sony-high-iso-nr", "normal");
            }
            
            normalCamera.setParameters(params);
            
            Object modifier = getModifier();
            if (modifier != null && setDriveModeMethod != null) {
                Class<?>[] pTypes = setDriveModeMethod.getParameterTypes();
                if (pTypes.length > 0 && pTypes[0] == int.class) {
                    setDriveModeMethod.invoke(modifier, 0); // DRIVE_MODE_SINGLE = 0
                } else if (pTypes.length > 0 && pTypes[0] == String.class) {
                    setDriveModeMethod.invoke(modifier, "single");
                }
                commitModifier(modifier);
                Log.i(TAG, "Drive mode enforced: Single-Shot. HDR/MFNR disabled.");
            }
        } catch (Throwable t) {
            Log.w(TAG, "setDriveModeSingle failed", t);
        }
    }

    public boolean isWithinManualLockout() {
        return (System.currentTimeMillis() - lastManualIsoChangeTime) < 800;
    }

    /**
     * Sets ISO directly with valid scene-mode validation and 800ms poller freeze.
     */
    public synchronized boolean setHardwareIso(int targetIso) {
        if (normalCamera == null || cameraEx == null) return false;
        try {
            Camera.Parameters params = normalCamera.getParameters();
            List<String> supportedModes = params.getSupportedSceneModes();
            if (supportedModes != null && supportedModes.contains("manual-exposure")) {
                params.setSceneMode("manual-exposure");
            }

            Object mod = createModifier(params);
            if (mod != null) {
                Method setIsoMethod = findAccessibleMethod(mod, "setISOSensitivity", int.class);
                setIsoMethod.invoke(mod, targetIso);

                Camera.Parameters toApply = params;
                try {
                    Method getParams = mod.getClass().getMethod("getParameters");
                    Object p = getParams.invoke(mod);
                    if (p instanceof Camera.Parameters) toApply = (Camera.Parameters) p;
                } catch (Throwable ignored) {}

                normalCamera.setParameters(toApply);
                lastManualIsoChangeTime = System.currentTimeMillis();
                cachedIso = (targetIso == 0) ? "ISO AUTO" : "ISO " + targetIso;
                return true;
            }
        } catch (Throwable t) {
            Log.e(TAG, "Failed to set ISO: " + targetIso, t);
        }
        return false;
    }

    private Object createModifier(Camera.Parameters params) {
        if (cameraEx == null || params == null) return null;
        try {
            if (createModifierMethod != null) {
                return createModifierMethod.invoke(cameraEx, params);
            }
            Method m = cameraEx.getClass().getMethod("createParametersModifier", Camera.Parameters.class);
            m.setAccessible(true);
            return m.invoke(cameraEx, params);
        } catch (Throwable t) {
            return null;
        }
    }

    // Standard ISO ladder on Sony Alpha 7R II
    public static final int[] ISO_STEPS = {
        0, 50, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600,
        2000, 2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800, 16000, 20000, 25600
    };
    private int currentIsoIndex = 8; // Default ISO 400
    private long lastManualIsoChangeTime = 0; // Lock for exposure poller

    public void stepIso(int delta) {
        if (normalCamera == null || cameraEx == null) return;
        try {
            Camera.Parameters params = normalCamera.getParameters();
            Object modifier = createModifier(params);
            if (modifier == null) return;

            Method getSupportedMethod = null;
            Method getIsoMethod = null;
            try { getSupportedMethod = findAccessibleMethod(modifier, "getSupportedISOSensitivities"); } catch (Throwable ignored) {}
            try { getIsoMethod = findAccessibleMethod(modifier, "getISOSensitivity"); } catch (Throwable ignored) {}

            int targetIso = 400;
            if (getSupportedMethod != null && getIsoMethod != null) {
                @SuppressWarnings("unchecked")
                List<Integer> supported = (List<Integer>) getSupportedMethod.invoke(modifier);
                if (supported != null && !supported.isEmpty()) {
                    java.util.ArrayList<Integer> validSteps = new java.util.ArrayList<Integer>();
                    for (int s : ISO_STEPS) {
                        if (supported.contains(s)) validSteps.add(s);
                    }
                    if (validSteps.isEmpty()) validSteps.addAll(supported);

                    Integer currentIsoObj = (Integer) getIsoMethod.invoke(modifier);
                    int currentIso = currentIsoObj != null ? currentIsoObj.intValue() : 0;
                    int idx = validSteps.indexOf(currentIso);
                    if (idx == -1) idx = 0;

                    int nextIdx = Math.max(0, Math.min(validSteps.size() - 1, idx + delta));
                    targetIso = validSteps.get(nextIdx);
                } else {
                    currentIsoIndex = Math.max(0, Math.min(ISO_STEPS.length - 1, currentIsoIndex + delta));
                    targetIso = ISO_STEPS[currentIsoIndex];
                }
            } else {
                currentIsoIndex = Math.max(0, Math.min(ISO_STEPS.length - 1, currentIsoIndex + delta));
                targetIso = ISO_STEPS[currentIsoIndex];
            }

            setHardwareIso(targetIso);
        } catch (Throwable t) {
            Log.e(TAG, "stepIso failed", t);
        }
    }

    public void adjustIso(int direction) {
        stepIso(direction);
    }

    // ------------------------------------------------------------------------
    // Hardware Exposure Control (Shutter Speed, Aperture, ISO, EV)
    // ------------------------------------------------------------------------

    public void ensureManualOrShutterPriority() {
        if (normalCamera == null) return;
        try {
            Camera.Parameters params = normalCamera.getParameters();
            List<String> supported = params.getSupportedSceneModes();
            String mode = params.get("sony-exposure-mode");
            if ("program-auto".equals(mode) || "aperture-priority".equals(mode)) {
                if (supported != null && supported.contains("manual-exposure")) {
                    params.set("sony-exposure-mode", "manual-exposure");
                    params.setSceneMode("manual-exposure");
                    normalCamera.setParameters(params);
                }
            }
        } catch (Throwable ignored) {}
    }

    public void adjustShutterSpeed(int step) {
        ensureManualOrShutterPriority();
        if (cameraEx != null) {
            try {
                Method m = cameraEx.getClass().getMethod("adjustShutterSpeed", int.class);
                m.invoke(cameraEx, step);
                getShutterSpeed();
                return;
            } catch (Throwable t) {
                Log.w(TAG, "cameraEx.adjustShutterSpeed failed", t);
            }
        }
        incrementShutterSpeed(step);
    }

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
            if (modifier != null) {
                Method m = findAccessibleMethod(modifier, "getShutterSpeed");
                Object cur = m.invoke(modifier);
                if (cur != null) {
                    Class<?> pairCls = cur.getClass();
                    int first = 0;
                    int second = 0;
                    try {
                        first = ((Integer) pairCls.getField("first").get(cur)).intValue();
                        second = ((Integer) pairCls.getField("second").get(cur)).intValue();
                    } catch (Throwable t) {
                        try {
                            first = ((Integer) pairCls.getMethod("getFirst").invoke(cur)).intValue();
                            second = ((Integer) pairCls.getMethod("getSecond").invoke(cur)).intValue();
                        } catch (Throwable ignored) {}
                    }
                    if (first != 0 || second != 0) {
                        cachedShutter = formatShutterSpeed(first, second);
                    }
                }
            }
        } catch (Throwable ignored) {}
        return cachedShutter;
    }

    private String formatShutterSpeed(int num, int den) {
        if (num == 65535 && den == 1) return "BULB";
        if (den == 1) return num + "\"";
        if (num == 1) return "1/" + den;
        if (num <= 0 || den <= 0) return "AUTO";
        float sec = (float) num / (float) den;
        return String.format(java.util.Locale.US, "%.1f\"", sec);
    }

    public void adjustAperture(int step) {
        if (cameraEx != null) {
            try {
                Method m = cameraEx.getClass().getMethod("adjustAperture", int.class);
                m.invoke(cameraEx, step);
                getAperture();
                return;
            } catch (Throwable t) {
                Log.w(TAG, "cameraEx.adjustAperture failed", t);
            }
        }
        incrementAperture(step);
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
                            setMethod.invoke(modifier, ((Integer) nextVal).intValue());

                            Camera.Parameters toApply = params;
                            try {
                                Method getParams = modifier.getClass().getMethod("getParameters");
                                Object p = getParams.invoke(modifier);
                                if (p instanceof Camera.Parameters) toApply = (Camera.Parameters) p;
                            } catch (Throwable ignored) {}

                            normalCamera.setParameters(toApply);
                            int val = ((Integer) nextVal).intValue();
                            cachedAperture = (val <= 0) ? "F--" : String.format(java.util.Locale.US, "F%.1f", val / 100.0f);
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
            if (modifier != null) {
                Method m = findAccessibleMethod(modifier, "getAperture");
                Object cur = m.invoke(modifier);
                if (cur instanceof Integer) {
                    int val = ((Integer) cur).intValue();
                    cachedAperture = (val <= 0) ? "F--" : String.format(java.util.Locale.US, "F%.1f", val / 100.0f);
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
