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

    public interface StoreImageListener {
        void onImageStored(String dir, String filename);
    }
    private volatile StoreImageListener storeImageListener;
    private volatile Camera.ShutterCallback activeShutterCallback;

    public void setStoreImageListener(StoreImageListener listener) {
        this.storeImageListener = listener;
    }

    // Local cached hardware states for Zero-GC, Zero-IPC HUD updates
    private volatile String cachedShutter = "1/125";
    private volatile String cachedAperture = "F2.8";
    private volatile String cachedIso = "ISO 400";
    private volatile String cachedEv = "+0.0";
    private volatile boolean aelLocked = false;
    private int afModeIndex = 0; // 0: AF-S, 1: AF-C, 2: MF
    private static final String[] AF_MODES = {"AF-S", "AF-C", "MF"};

    // Cached reflection lookups (eliminates per-call Class.forName + getMethod overhead)
    private static volatile Class<?> sCachedModifierIface = null;
    private static volatile boolean sModifierIfaceLookedUp = false;
    private static final java.util.HashMap<String, Method> sModifierMethodCache = new java.util.HashMap<String, Method>();

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
                    configureFullResolutionCapture();
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
            if (normalCamera != null) {
                configureFullResolutionCapture();
            }
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

        StringBuilder keySb = new StringBuilder(64);
        keySb.append(target.getClass().getName()).append('#').append(name);
        if (paramTypes != null) {
            for (Class<?> pt : paramTypes) {
                keySb.append(':').append(pt.getName());
            }
        }
        String cacheKey = keySb.toString();
        synchronized (sModifierMethodCache) {
            Method cached = sModifierMethodCache.get(cacheKey);
            if (cached != null) return cached;
        }

        if (!sModifierIfaceLookedUp) {
            try {
                sCachedModifierIface = Class.forName("com.sony.scalar.hardware.CameraEx$ParametersModifier");
            } catch (Throwable ignored) {}
            sModifierIfaceLookedUp = true;
        }

        // 1. Try public interface first
        Method resolved = null;
        try {
            Class<?> iface = sCachedModifierIface;
            if (iface != null && iface.isInstance(target)) {
                resolved = iface.getMethod(name, paramTypes);
                resolved.setAccessible(true);
            }
        } catch (Throwable ignored) {}

        // 2. Fallback to implementation class hierarchy
        if (resolved == null) {
            Class<?> clazz = target.getClass();
            while (clazz != null && resolved == null) {
                try {
                    resolved = clazz.getDeclaredMethod(name, paramTypes);
                    resolved.setAccessible(true);
                } catch (NoSuchMethodException e) {
                    try {
                        resolved = clazz.getMethod(name, paramTypes);
                        resolved.setAccessible(true);
                    } catch (NoSuchMethodException ignored) {}
                }
                clazz = clazz.getSuperclass();
            }
        }

        if (resolved == null) {
            throw new NoSuchMethodException(name);
        }
        synchronized (sModifierMethodCache) {
            sModifierMethodCache.put(cacheKey, resolved);
        }
        return resolved;
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
                        triggerAsyncTelemetryRefresh();
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
                        triggerAsyncTelemetryRefresh();
                        return null;
                    }
                }
            );
            Method setApListener = cls.getMethod("setApertureChangeListener", apListenerCls);
            setApListener.setAccessible(true);
            setApListener.invoke(cameraEx, apProxy);

            Log.i(TAG, "Hardware dial listeners registered successfully via Proxy");

            // Hardware StoreImageCompleteListener (notifies when BIONZ X finishes writing full-res image to SD)
            try {
                Class<?> storeListenerCls = Class.forName("com.sony.scalar.hardware.CameraEx$StoreImageCompleteListener");
                Object storeProxy = java.lang.reflect.Proxy.newProxyInstance(
                    storeListenerCls.getClassLoader(),
                    new Class<?>[]{storeListenerCls},
                    new java.lang.reflect.InvocationHandler() {
                        @Override
                        public Object invoke(Object proxy, Method method, Object[] args) {
                            if ("onDone".equals(method.getName()) && args != null && args.length > 0) {
                                try {
                                    String dir = null;
                                    String fn = null;
                                    for (Object arg : args) {
                                        if (arg == null) continue;
                                        try {
                                            try { dir = (String) arg.getClass().getField("DirectoryName").get(arg); } catch (Throwable ignored) {}
                                            try { fn = (String) arg.getClass().getField("FileName").get(arg); } catch (Throwable ignored) {}
                                            if (dir == null) {
                                                try { dir = (String) arg.getClass().getMethod("getDirectoryName").invoke(arg); } catch (Throwable ignored) {}
                                            }
                                            if (fn == null) {
                                                try { fn = (String) arg.getClass().getMethod("getFileName").invoke(arg); } catch (Throwable ignored) {}
                                            }
                                        } catch (Throwable ignored) {}
                                        if (dir != null && fn != null) break;
                                    }
                                    Log.i(TAG, "Hardware StoreImageCompleteListener: " + dir + "/" + fn);
                                    if (storeImageListener != null) {
                                        storeImageListener.onImageStored(dir, fn);
                                    }
                                } catch (Throwable t) {
                                    Log.w(TAG, "Failed reading StoreImageInfo", t);
                                }
                            }
                            return null;
                        }
                    }
                );
                Method setStoreListener = cls.getMethod("setStoreImageCompleteListener", storeListenerCls);
                setStoreListener.setAccessible(true);
                setStoreListener.invoke(cameraEx, storeProxy);
                Log.i(TAG, "StoreImageCompleteListener registered successfully");
            } catch (Throwable t) {
                Log.w(TAG, "StoreImageCompleteListener unsupported or failed to register", t);
            }

            // CameraEx.ShutterListener (LightLab mechanism: handles burstableTakePicture shutter sync)
            try {
                Class<?> shutterListenerCls = Class.forName("com.sony.scalar.hardware.CameraEx$ShutterListener");
                Object shutterProxy = java.lang.reflect.Proxy.newProxyInstance(
                    shutterListenerCls.getClassLoader(),
                    new Class<?>[]{shutterListenerCls},
                    new java.lang.reflect.InvocationHandler() {
                        @Override
                        public Object invoke(Object proxy, Method method, Object[] args) {
                            String mName = method.getName();
                            if ("onShutter".equals(mName)) {
                                Log.i(TAG, "Hardware CameraEx.ShutterListener.onShutter received");
                                // Dispatch to active shutter callback (Do NOT call cancelTakePicture here as it aborts exposure!)
                                Camera.ShutterCallback cb = activeShutterCallback;
                                if (cb != null) {
                                    activeShutterCallback = null;
                                    cb.onShutter();
                                }
                            }
                            return null;
                        }
                    }
                );
                Method setShutterListenerMethod = cls.getMethod("setShutterListener", shutterListenerCls);
                setShutterListenerMethod.setAccessible(true);
                setShutterListenerMethod.invoke(cameraEx, shutterProxy);
                Log.i(TAG, "CameraEx.ShutterListener registered successfully");
            } catch (Throwable t) {
                Log.w(TAG, "CameraEx.ShutterListener registration unsupported or failed", t);
            }
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
    private Runnable onHardwareChangeCallback;
    private android.os.HandlerThread pollerThread;
    private android.os.Handler pollerHandler;
    private final android.os.Handler mainUiHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private volatile boolean isPolling = false;
    private volatile boolean pollingPaused = false;
    private static final long HARDWARE_POLL_INTERVAL_MS = 500L; // 2Hz background poll (down from 10Hz UI-thread storm)

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

    private synchronized void ensureBackgroundHandler() {
        if (pollerThread == null || !pollerThread.isAlive()) {
            pollerThread = new android.os.HandlerThread(
                "FujiLite-HwPoller", android.os.Process.THREAD_PRIORITY_BACKGROUND);
            pollerThread.start();
            pollerHandler = new android.os.Handler(pollerThread.getLooper());
        }
    }

    /**
     * Dispatches a hardware parameter update (e.g. setParameters) off the Main UI thread
     * so Sony BIONZ X ISP reconfiguration never freezes UI input or live view.
     */
    public void postHardwareTask(Runnable task) {
        if (task == null) return;
        try {
            ensureBackgroundHandler();
            if (pollerHandler != null) {
                pollerHandler.post(task);
                return;
            }
        } catch (Throwable ignored) {}
        task.run();
    }

    public void setPollingPaused(boolean paused) {
        this.pollingPaused = paused;
    }

    private final Runnable immediateRefreshRunnable = new Runnable() {
        @Override
        public void run() {
            if (pollingPaused) return;
            if (refreshExposureTelemetry() && onHardwareChangeCallback != null) {
                mainUiHandler.post(onHardwareChangeCallback);
            }
        }
    };

    public void triggerAsyncTelemetryRefresh() {
        if (pollerHandler != null) {
            pollerHandler.removeCallbacks(immediateRefreshRunnable);
            pollerHandler.postDelayed(immediateRefreshRunnable, 60);
        }
    }

    /**
     * Refreshes Shutter Speed, Aperture, ISO, and EV from a SINGLE Camera.getParameters()
     * Binder IPC snapshot on the background thread (replaces 3-6 separate UI-thread IPCs per tick).
     */
    public boolean refreshExposureTelemetry() {
        Camera cam = normalCamera;
        Object camEx = cameraEx;
        if (cam == null) return false;

        String prevS = cachedShutter;
        String prevA = cachedAperture;
        String prevI = cachedIso;
        String prevE = cachedEv;

        try {
            Camera.Parameters params = cam.getParameters();
            if (params == null) return false;

            try {
                int evIdx = params.getExposureCompensation();
                float step = params.getExposureCompensationStep();
                if (step > 0f) {
                    float evVal = evIdx * step;
                    cachedEv = (evVal >= 0 ? "+" : "") + String.format(java.util.Locale.US, "%.1f", evVal);
                }
            } catch (Throwable ignored) {}

            Object modifier = (camEx != null) ? createModifier(params) : null;
            if (modifier != null) {
                // 1. Shutter Speed
                try {
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
                } catch (Throwable ignored) {}

                // 2. Aperture
                try {
                    Method m = findAccessibleMethod(modifier, "getAperture");
                    Object cur = m.invoke(modifier);
                    if (cur instanceof Integer) {
                        int val = ((Integer) cur).intValue();
                        cachedAperture = (val <= 0) ? "F--" : String.format(java.util.Locale.US, "F%.1f", val / 100.0f);
                    }
                } catch (Throwable ignored) {}

                // 3. ISO Sensitivity (previously dead because getISOSensitivityMethod was never initialized)
                try {
                    Method m = findAccessibleMethod(modifier, "getISOSensitivity");
                    Object cur = m.invoke(modifier);
                    if (cur instanceof Integer) {
                        int iso = ((Integer) cur).intValue();
                        cachedIso = (iso == 0) ? "ISO AUTO" : "ISO " + iso;
                    }
                } catch (Throwable ignored) {}
            }
        } catch (Throwable ignored) {
            return false;
        }

        return !cachedShutter.equals(prevS)
            || !cachedAperture.equals(prevA)
            || !cachedIso.equals(prevI)
            || !cachedEv.equals(prevE);
    }

    public void startHardwarePoller(final Runnable onHardwareChange) {
        this.onHardwareChangeCallback = onHardwareChange;
        if (isPolling) return;
        ensureBackgroundHandler();

        isPolling = true;
        hardwarePoller = new Runnable() {
            @Override
            public void run() {
                if (!isPolling) return;

                if (!pollingPaused && (System.currentTimeMillis() - lastManualIsoChangeTime >= 800)) {
                    if (refreshExposureTelemetry() && onHardwareChangeCallback != null) {
                        mainUiHandler.post(onHardwareChangeCallback);
                    }
                }

                if (isPolling && pollerHandler != null) {
                    pollerHandler.postDelayed(this, HARDWARE_POLL_INTERVAL_MS);
                }
            }
        };
        pollerHandler.postDelayed(hardwarePoller, 250L);
        Log.i(TAG, "2Hz Background Hardware Exposure Poller started (off UI thread)");
    }

    public void stopHardwarePoller() {
        isPolling = false;
        if (pollerHandler != null) {
            if (hardwarePoller != null) pollerHandler.removeCallbacks(hardwarePoller);
            pollerHandler.removeCallbacks(immediateRefreshRunnable);
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
    public void takePicture(final Camera.PictureCallback jpegCallback) {
        if (!ensureOpen()) return;
        try {
            if (jpegCallback == null) {
                normalCamera.takePicture(null, null, null);
            } else {
                Camera.ShutterCallback shutterCb = new Camera.ShutterCallback() {
                    @Override
                    public void onShutter() {
                        Log.i(TAG, "Hardware shutter callback fired");
                    }
                };
                try {
                    normalCamera.takePicture(shutterCb, null, jpegCallback);
                    Log.i(TAG, "takePicture(shutterCb, null, jpegCallback) dispatched");
                } catch (Throwable t1) {
                    Log.w(TAG, "takePicture with shutterCb failed, falling back to (null, null, callback)", t1);
                    normalCamera.takePicture(null, null, jpegCallback);
                }
            }
        } catch (Throwable t) {
            Log.e(TAG, "takePicture failed", t);
        }
    }

    /**
     * Triggers full-resolution hardware capture by passing (shutter, null, null).
     * On Sony PMCA bodies (A7R II), CameraEx.burstableTakePicture() paired with
     * CameraEx.ShutterListener halts burst at 1 frame via cancelTakePicture() and
     * directs BIONZ X to write the full 42.4MP master JPEG directly to SD card.
     */
    public void takeHardwarePicture(final Camera.ShutterCallback shutterCallback) {
        if (!ensureOpen()) return;

        // 1. Preferred Sony PMCA method (proven LightLab hardware trigger): CameraEx.burstableTakePicture()
        if (cameraEx != null) {
            try {
                Method burstM = cameraEx.getClass().getMethod("burstableTakePicture");
                burstM.setAccessible(true);
                this.activeShutterCallback = shutterCallback;
                burstM.invoke(cameraEx);
                Log.i(TAG, "Hardware CameraEx.burstableTakePicture() invoked successfully");
                return;
            } catch (Throwable t) {
                Log.w(TAG, "CameraEx.burstableTakePicture() unavailable; trying normalCamera.takePicture", t);
                this.activeShutterCallback = null;
            }
        }

        // 2. Direct hardware capture fallback: normalCamera.takePicture(shutterCallback, null, null)
        try {
            normalCamera.takePicture(shutterCallback, null, null);
            Log.i(TAG, "Hardware normalCamera.takePicture(shutterCallback, null, null) invoked");
            return;
        } catch (Throwable t) {
            Log.w(TAG, "takePicture(shutterCallback, null, null) failed; trying (null, null, null)", t);
        }

        // 3. Last-resort fallback: normalCamera.takePicture(null, null, null)
        try {
            normalCamera.takePicture(null, null, null);
            Log.i(TAG, "Hardware takePicture(null, null, null) invoked");
            if (shutterCallback != null) {
                shutterCallback.onShutter();
            }
        } catch (Throwable t2) {
            Log.e(TAG, "takePicture(null, null, null) failed", t2);
            cancelTakePicture();
        }
    }

    @Override
    public boolean resumePreview() {
        if (normalCamera == null) return false;
        try {
            normalCamera.startPreview();
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "resumePreview failed", t);
            return false;
        }
    }

    /**
     * Interrupts BIONZ X exposure and resets hardware trigger logic.
     */
    public void cancelTakePicture() {
        activeShutterCallback = null;
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

    /**
     * Configures the camera sensor and BIONZ X ISP for native 42.4MP JPEG capture.
     * On Sony Alpha 7R II, setting ImageSize="large" (7952x5304) directs the BIONZ X ASIC
     * to capture full native 42.4MP resolution.
     */
    public void configureFullResolutionCapture() {
        if (normalCamera == null) return;

        // 1. Standard Android Camera.Parameters: target native 42.4MP (7952x5304)
        try {
            Camera.Parameters params = normalCamera.getParameters();
            List<Camera.Size> sizes = params.getSupportedPictureSizes();
            if (sizes != null && !sizes.isEmpty()) {
                Camera.Size optimal = null;
                // Target: Native 42.4MP (7952x5304) or maximum 3:2 size
                for (Camera.Size s : sizes) {
                    if (s.width >= 7000) {
                        optimal = s;
                        break;
                    }
                }
                if (optimal == null) {
                    for (Camera.Size s : sizes) {
                        if (optimal == null || (s.width * s.height > optimal.width * optimal.height)) {
                            optimal = s;
                        }
                    }
                }
                if (optimal != null) {
                    Log.i(TAG, "Configured Camera picture-size: " + optimal.width + "x" + optimal.height);
                    params.setPictureSize(optimal.width, optimal.height);
                    normalCamera.setParameters(params);
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed setting picture size on standard Camera parameters", t);
        }

        // 2. Sony CameraEx ParametersModifier: set image size to "large" (42.4MP on A7R II Full Frame)
        if (cameraEx != null) {
            try {
                Object mod = getModifier();
                if (mod != null) {
                    try {
                        String chosenSize = "large";
                        try {
                            Method getSizes = findAccessibleMethod(mod, "getSupportedImageSizes");
                            @SuppressWarnings("unchecked")
                            List<String> supSizes = (List<String>) getSizes.invoke(mod);
                            if (supSizes != null && !supSizes.isEmpty()) {
                                for (String sz : supSizes) {
                                    if ("L".equalsIgnoreCase(sz) || "large".equalsIgnoreCase(sz)) {
                                        chosenSize = sz;
                                        break;
                                    }
                                }
                            }
                        } catch (Throwable ignored) {}
                        Method m = findAccessibleMethod(mod, "setImageSize", String.class);
                        // On Sony A7R II Full Frame, "L" / "large" = 42.4 MP (7952x5304)
                        m.invoke(mod, chosenSize);
                        Log.i(TAG, "CameraEx modifier.setImageSize('" + chosenSize + "') applied (42.4MP target)");
                    } catch (Throwable ignored) {}

                    try {
                        Method m = findAccessibleMethod(mod, "setImageAspectRatio", String.class);
                        m.invoke(mod, "3:2");
                        Log.i(TAG, "CameraEx modifier.setImageAspectRatio('3:2') applied");
                    } catch (Throwable ignored) {}

                    try {
                        Method m = findAccessibleMethod(mod, "setPictureStorageFormat", String.class);
                        m.invoke(mod, "jpeg");
                        Log.i(TAG, "CameraEx modifier.setPictureStorageFormat('jpeg') applied");
                    } catch (Throwable ignored) {}

                    commitModifier(mod);
                }
            } catch (Throwable t) {
                Log.w(TAG, "Failed configuring CameraEx parameters modifier for 42.4MP", t);
            }
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

            // Apply directly on the already-fetched params & modifier (saves 1 full getParameters() Binder IPC per dial click)
            List<String> supportedModes = params.getSupportedSceneModes();
            if (supportedModes != null && supportedModes.contains("manual-exposure")) {
                params.setSceneMode("manual-exposure");
            }
            Method setIsoMethod = findAccessibleMethod(modifier, "setISOSensitivity", int.class);
            setIsoMethod.invoke(modifier, targetIso);

            Camera.Parameters toApply = params;
            try {
                Method getParams = findAccessibleMethod(modifier, "getParameters");
                Object p = getParams.invoke(modifier);
                if (p instanceof Camera.Parameters) toApply = (Camera.Parameters) p;
            } catch (Throwable ignored) {}

            normalCamera.setParameters(toApply);
            lastManualIsoChangeTime = System.currentTimeMillis();
            cachedIso = (targetIso == 0) ? "ISO AUTO" : "ISO " + targetIso;
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
                triggerAsyncTelemetryRefresh();
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

    /** Zero-IPC cached getter for UI HUD (populated by background poller / listeners). */
    public String getShutterSpeed() {
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
                triggerAsyncTelemetryRefresh();
                return;
            } catch (Throwable t) {
                Log.w(TAG, "cameraEx.adjustAperture failed", t);
            }
        }
        incrementAperture(step);
    }

    public void incrementAperture(int step) {
        if (normalCamera == null) return;
        try {
            Camera.Parameters params = normalCamera.getParameters();
            Object modifier = createModifier(params);
            if (modifier != null) {
                Method getSupportedMethod = null;
                Method getCurMethod = null;
                Method setMethod = null;
                try { getSupportedMethod = findAccessibleMethod(modifier, "getSupportedApertures"); } catch (Throwable ignored) {}
                try { getCurMethod = findAccessibleMethod(modifier, "getAperture"); } catch (Throwable ignored) {}
                try { setMethod = findAccessibleMethod(modifier, "setAperture", int.class); } catch (Throwable ignored) {}

                if (getSupportedMethod != null && getCurMethod != null && setMethod != null) {
                    List<?> supported = (List<?>) getSupportedMethod.invoke(modifier);
                    Object cur = getCurMethod.invoke(modifier);
                    if (supported != null && !supported.isEmpty() && cur instanceof Integer) {
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
            }
        } catch (Throwable t) {
            Log.e(TAG, "incrementAperture failed", t);
        }

        cachedAperture = (step > 0) ? "F4.0" : "F2.0";
    }

    /** Zero-IPC cached getter for UI HUD (populated by background poller / listeners). */
    public String getAperture() {
        return cachedAperture;
    }

    public void setISOSensitivity(int iso) {
        try {
            Object modifier = getModifier();
            if (modifier != null) {
                Method setIso = findAccessibleMethod(modifier, "setISOSensitivity", int.class);
                setIso.invoke(modifier, iso);
                commitModifier(modifier);
                cachedIso = (iso == 0) ? "ISO AUTO" : "ISO " + iso;
            }
        } catch (Throwable ignored) {}
    }

    /** Zero-IPC cached getter for UI HUD (populated by background poller / listeners). */
    public String getIso() {
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
        stopHardwarePoller();
        if (normalCamera != null) {
            try {
                normalCamera.setPreviewCallbackWithBuffer(null);
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

    public synchronized void shutdownPollerThread() {
        stopHardwarePoller();
        if (pollerThread != null) {
            try {
                pollerThread.quit();
            } catch (Throwable ignored) {}
            pollerThread = null;
            pollerHandler = null;
        }
    }

    @Override
    public boolean supportsInternalColorPipeline() {
        return false;
    }
}
