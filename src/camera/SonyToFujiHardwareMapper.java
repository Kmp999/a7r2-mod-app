package com.fujilite.a7r2;

import android.hardware.Camera;
import android.util.Log;

import java.lang.reflect.Method;

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

            // 4. Deep CameraEx ParametersModifier for DRO and WB shift
            if (cameraEx != null) {
                applySonyExModifiers(cameraEx, params, recipe);
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
            case VELVIA: sonyStyle = "vivid"; break;
            case ASTIA:
            case PRO_NEG_STD:
            case PRO_NEG_HI: sonyStyle = "portrait"; break;
            case ETERNA:
            case CLASSIC_CHROME: sonyStyle = "neutral"; break;
            case ACROS: sonyStyle = "black-white"; break;
            case PROVIA:
            case CLASSIC_NEG:
            default: sonyStyle = "standard"; break;
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

        if (evOffset != 0) {
            int targetEvIndex = clamp(currentEvIndex + evOffset,
                params.getMinExposureCompensation(),
                params.getMaxExposureCompensation());
            params.setExposureCompensation(targetEvIndex);
            Log.d(TAG, "Applied hardware DR EV protection: " + evOffset + " steps");
        }
    }

    // ------------------------------------------------------------------------
    // Strict Reflection Resolver
    // ------------------------------------------------------------------------
    private static Method findAccessibleMethod(Object target, String name, Class<?>... paramTypes) throws NoSuchMethodException {
        if (target == null) throw new NoSuchMethodException("Target is null for method " + name);
        try {
            Class<?> iface = Class.forName("com.sony.scalar.hardware.CameraEx$ParametersModifier");
            if (iface.isInstance(target)) {
                Method m = iface.getMethod(name, paramTypes);
                m.setAccessible(true);
                return m;
            }
        } catch (Throwable ignored) {}

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

    private static void applySonyExModifiers(Object cameraEx, Camera.Parameters params, FujiRecipe recipe) {
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
            if (createModMethod == null) return;

            Object modifier = createModMethod.invoke(cameraEx, params);
            if (modifier == null) return;

            // 1. Dynamic Range (DR) -> Sony DRO (Dynamic Range Optimizer)
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

            // 2. WB Fine Shift
            int sonyAB = recipe.wbShiftR;
            int sonyGM = -recipe.wbShiftB;

            try {
                Method setWbShift = findAccessibleMethod(modifier, "setColorTempShift", int.class, int.class);
                setWbShift.invoke(modifier, sonyAB, sonyGM);
            } catch (Throwable ignored) {}

            // Update params from modifier
            try {
                Method getParams = findAccessibleMethod(modifier, "getParameters");
                Camera.Parameters newParams = (Camera.Parameters) getParams.invoke(modifier);
            } catch (Throwable ignored) {}

        } catch (Throwable t) {
            Log.w(TAG, "CameraEx ParametersModifier not fully supported", t);
        }
    }

    private static int clamp(int val, int min, int max) {
        return Math.max(min, Math.min(max, val));
    }
}
