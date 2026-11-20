package com.fujilite.a7r2;

import android.hardware.Camera;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.List;

/**
 * Maps Fujifilm Recipe parameters directly to Sony BIONZ X hardware registers
 * via com.sony.scalar.hardware.CameraEx and Camera.Parameters.
 * Operates at Stage 1 (0ms hardware latency) before image capture.
 */
public class SonyToFujiHardwareMapper {
    private static final String TAG = "SonyToFujiMapper";

    /**
     * Applies the FujiRecipe settings directly to Sony camera hardware.
     *
     * @param cameraEx com.sony.scalar.hardware.CameraEx instance (via reflection), or null
     * @param camera   android.hardware.Camera instance
     * @param recipe   Current Fujifilm recipe configuration
     */
    public static void applyRecipeToHardware(Object cameraEx, Camera camera, FujiRecipe recipe) {
        if (camera == null || recipe == null) return;

        try {
            Camera.Parameters params = camera.getParameters();

            // 1. Creative Style base
            mapCreativeStyle(params, recipe.filmSim);

            // 2. Hardware Contrast, Sharpness, Saturation
            mapToningAndSharpness(params, recipe);

            // 3. Dynamic Range Hardware Exposure Protection (-1.0 EV for DR200, -2.0 EV for DR400)
            mapDynamicRangeHardware(params, recipe);

            // Step 2.1: RAW / RAW+JPEG Format Guardrails for Camera.Parameters
            // Only upgrade to raw+jpeg when camera is in RAW-only mode (avoids unnecessary 42-84MB .ARW SD flushes)
            try {
                params.set("sony-quality", "fine");
                String curSonyFormat = params.get("sony-picture-format");
                if ("raw".equalsIgnoreCase(curSonyFormat) || "arw".equalsIgnoreCase(curSonyFormat)) {
                    params.set("sony-picture-format", "raw+jpeg");
                }
                String curPicFormat = params.get("picture-format");
                if ("raw".equalsIgnoreCase(curPicFormat) || "arw".equalsIgnoreCase(curPicFormat)) {
                    params.set("picture-format", "raw+jpeg");
                }
            } catch (Throwable ignored) {}

            // 4. Deep CameraEx ParametersModifier for DRO, WB shift, 10.6MP ImageSize, and Single DriveMode
            if (cameraEx != null) {
                Camera.Parameters modifiedParams = applySonyExModifiers(cameraEx, params, recipe);
                if (modifiedParams != null) {
                    params = modifiedParams;
                }
            }

            // Write parameters to camera driver
            camera.setParameters(params);
            Log.i(TAG, "Hardware parameters applied for " + recipe.filmSim.label);

        } catch (Throwable t) {
            Log.e(TAG, "Failed to apply hardware mapping", t);
        }
    }

    private static void mapCreativeStyle(Camera.Parameters params, FujiRecipe.FilmSim sim) {
        String sonyStyle = "standard";
        switch (sim) {
            case VELVIA:
            case CINEMA_LANDSCAPE:
            case NEON_CYBERPUNK:
                sonyStyle = "vivid";
                break;
            case ASTIA:
            case PRO_NEG_STD:
            case PRO_NEG_HI:
            case KODAK_PORTRA_5205:
            case VINTAGE_PINK:
                sonyStyle = "portrait";
                break;
            case ETERNA:
            case CLASSIC_CHROME:
            case FUJI_ETERNA_250D:
            case FUJI_3510:
            case FUJI_3513DI:
            case KODAK_VISION_5218:
            case KODAK_2383_D60:
            case KODAK_2383_D55:
            case KODAK_2383_D65:
            case KODAK_FILMSTOCK_50:
            case TEAL_ORANGE:
            case TENSION_GREEN:
                sonyStyle = "neutral";
                break;
            case ACROS:
                sonyStyle = "black-white";
                break;
            case PROVIA:
            case CLASSIC_NEG:
            case FUJI_REALA_500D:
            case FUJI_F125:
            case AEROLUX:
            default:
                sonyStyle = "standard";
                break;
        }

        // ONLY use standard parameter if it works, removing fake keys to prevent RuntimeException
    }

    private static void mapToningAndSharpness(Camera.Parameters params, FujiRecipe recipe) {
        // Contrast: Combine highlight and shadow tones (-2..+4) -> (-3..+3)
        int avgTone = (recipe.highlightTone + recipe.shadowTone) / 2;
        int sonyContrast = clamp(avgTone, -3, 3);
        
        // Saturation (Color): (-4..+4) -> (-3..+3)
        int sonySaturation = clamp((recipe.color * 3) / 4, -3, 3);
        
        // Sharpness: (-4..+4) -> (-3..+3)
        int sonySharpness = clamp((recipe.sharpness * 3) / 4, -3, 3);
        
        // Removed fake sony-* parameters
    }

    private static int sLastAppliedDrEvOffset = 0;
    private static volatile Class<?> sCachedModifierIface = null;
    private static volatile boolean sModifierIfaceLookedUp = false;
    private static final java.util.HashMap<String, Method> sMethodCache = new java.util.HashMap<String, Method>();

    private static void mapDynamicRangeHardware(Camera.Parameters params, FujiRecipe recipe) {
        float step = params.getExposureCompensationStep();
        if (step <= 0f) step = 0.33333334f;
        int currentEvIndex = params.getExposureCompensation();

        // Calculate EV pull to protect sensor highlights:
        // DR200: -1.0 EV hardware pull; DR400: -2.0 EV hardware pull
        int evOffset = 0;
        if (recipe.dynamicRange == FujiRecipe.DynamicRange.DR200) {
            evOffset = -Math.round(1.0f / step);
        } else if (recipe.dynamicRange == FujiRecipe.DynamicRange.DR400) {
            evOffset = -Math.round(2.0f / step);
        }

        // Apply only the delta vs previous DR offset so repeated recipe updates don't compound EV down
        int delta = evOffset - sLastAppliedDrEvOffset;
        if (delta != 0) {
            int targetEvIndex = clamp(currentEvIndex + delta,
                params.getMinExposureCompensation(),
                params.getMaxExposureCompensation());
            params.setExposureCompensation(targetEvIndex);
            sLastAppliedDrEvOffset = evOffset;
            Log.d(TAG, "Applied hardware DR EV protection: " + evOffset + " steps (delta " + delta + ")");
        }
    }

    // ------------------------------------------------------------------------
    // Cached Reflection Resolver (Zero per-call Class.forName overhead)
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
        synchronized (sMethodCache) {
            Method cached = sMethodCache.get(cacheKey);
            if (cached != null) return cached;
        }

        if (!sModifierIfaceLookedUp) {
            try {
                sCachedModifierIface = Class.forName("com.sony.scalar.hardware.CameraEx$ParametersModifier");
            } catch (Throwable ignored) {}
            sModifierIfaceLookedUp = true;
        }

        Method resolved = null;
        try {
            Class<?> iface = sCachedModifierIface;
            if (iface != null && iface.isInstance(target)) {
                resolved = iface.getMethod(name, paramTypes);
                resolved.setAccessible(true);
            }
        } catch (Throwable ignored) {}

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
        synchronized (sMethodCache) {
            sMethodCache.put(cacheKey, resolved);
        }
        return resolved;
    }

    private static Camera.Parameters applySonyExModifiers(Object cameraEx, Camera.Parameters params, FujiRecipe recipe) {
        try {
            Class<?> camExClass = cameraEx.getClass();
            Method createModMethod = null;
            for (Method m : camExClass.getMethods()) {
                if ("createParametersModifier".equals(m.getName()) && m.getParameterTypes().length == 1) {
                    createModMethod = m;
                    createModMethod.setAccessible(true);
                    break;
                }
            }
            if (createModMethod == null) return params;

            Object modifier = createModMethod.invoke(cameraEx, params);
            if (modifier == null) return params;

            // 1. Preserve 42.4 MP (7952 x 5304) BIONZ X hardware capture and single-shot drive mode
            try {
                String chosenSize = "large";
                try {
                    Method getSizes = findAccessibleMethod(modifier, "getSupportedImageSizes");
                    @SuppressWarnings("unchecked")
                    List<String> supSizes = (List<String>) getSizes.invoke(modifier);
                    if (supSizes != null && !supSizes.isEmpty()) {
                        for (String sz : supSizes) {
                            if ("L".equalsIgnoreCase(sz) || "large".equalsIgnoreCase(sz)) {
                                chosenSize = sz;
                                break;
                            }
                        }
                    }
                } catch (Throwable ignored) {}
                Method setImgSize = findAccessibleMethod(modifier, "setImageSize", String.class);
                setImgSize.invoke(modifier, chosenSize);
            } catch (Throwable ignored) {}

            try {
                Method setAspect = findAccessibleMethod(modifier, "setImageAspectRatio", String.class);
                setAspect.invoke(modifier, "3:2");
            } catch (Throwable ignored) {}

            // Step 2.1: RAW / RAW+JPEG Format Guardrail - Enforce JPEG or RAW+JPEG so DSC0xxxx.JPG is guaranteed
            try {
                Method setFormat = findAccessibleMethod(modifier, "setPictureStorageFormat", String.class);
                String currentFormat = null;
                try {
                    Method getFormat = findAccessibleMethod(modifier, "getPictureStorageFormat");
                    if (getFormat != null) currentFormat = (String) getFormat.invoke(modifier);
                } catch (Throwable ignored) {}
                if ("raw".equalsIgnoreCase(currentFormat)) {
                    setFormat.invoke(modifier, "raw+jpeg");
                } else if (currentFormat == null || (!currentFormat.contains("jpeg") && !currentFormat.contains("raw+jpeg"))) {
                    setFormat.invoke(modifier, "jpeg");
                }
            } catch (Throwable ignored) {}

            try {
                Method setQuality = findAccessibleMethod(modifier, "setImageQuality", String.class);
                setQuality.invoke(modifier, "fine");
            } catch (Throwable ignored) {}

            try {
                Method setDrive = findAccessibleMethod(modifier, "setDriveMode", String.class);
                setDrive.invoke(modifier, "single");
            } catch (Throwable t) {
                try {
                    Method setDriveInt = findAccessibleMethod(modifier, "setDriveMode", int.class);
                    setDriveInt.invoke(modifier, 0);
                } catch (Throwable ignored) {}
            }

            // 2. Dynamic Range (DR) -> Sony DRO (Dynamic Range Optimizer)
            int droLevel = 1;
            switch (recipe.dynamicRange) {
                case DR100: droLevel = 1; break;
                case DR200: droLevel = 3; break;
                case DR400: droLevel = 5; break;
            }
            
            try {
                Method setDroMethod = findAccessibleMethod(modifier, "setDro", int.class);
                setDroMethod.invoke(modifier, droLevel);
            } catch (Throwable ignored) {}

            // Enforce minimum ISO baseline for dynamic range expansion (DR200: min ISO 200, DR400: min ISO 400)
            int minIso = (recipe.dynamicRange == FujiRecipe.DynamicRange.DR400) ? 400 :
                         (recipe.dynamicRange == FujiRecipe.DynamicRange.DR200) ? 200 : 100;
            try {
                Method getIso = findAccessibleMethod(modifier, "getISOSensitivity");
                Method setIso = findAccessibleMethod(modifier, "setISOSensitivity", int.class);
                Object curIsoObj = getIso.invoke(modifier);
                if (curIsoObj instanceof Integer) {
                    int curIso = ((Integer) curIsoObj).intValue();
                    if (curIso > 0 && curIso < minIso) {
                        setIso.invoke(modifier, minIso);
                        Log.i(TAG, "Enforced min ISO " + minIso + " for " + recipe.dynamicRange.label);
                    }
                }
            } catch (Throwable ignored) {}

            // 3. WB Fine Shift
            int sonyAB = recipe.wbShiftR;
            int sonyGM = -recipe.wbShiftB;

            try {
                Method setWbShift = findAccessibleMethod(modifier, "setColorTempShift", int.class, int.class);
                setWbShift.invoke(modifier, sonyAB, sonyGM);
            } catch (Throwable ignored) {}

            // Return updated parameters from modifier
            try {
                Method getParams = findAccessibleMethod(modifier, "getParameters");
                Camera.Parameters newParams = (Camera.Parameters) getParams.invoke(modifier);
                if (newParams != null) {
                    return newParams;
                }
            } catch (Throwable ignored) {}

        } catch (Throwable t) {
            Log.w(TAG, "CameraEx ParametersModifier not fully supported", t);
        }
        return params;
    }

    private static int clamp(int val, int min, int max) {
        return Math.max(min, Math.min(max, val));
    }
}
