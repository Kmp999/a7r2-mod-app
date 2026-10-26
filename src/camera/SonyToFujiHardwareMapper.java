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

            // 3. Deep CameraEx ParametersModifier for DRO and WB shift
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
            case VELVIA:
                sonyStyle = "vivid";
                break;
            case ASTIA:
            case PRO_NEG_STD:
            case PRO_NEG_HI:
                sonyStyle = "portrait";
                break;
            case ETERNA:
            case CLASSIC_CHROME:
                sonyStyle = "neutral";
                break;
            case ACROS:
                sonyStyle = "black-white";
                break;
            case PROVIA:
            case CLASSIC_NEG:
            default:
                sonyStyle = "standard";
                break;
        }

        params.set("sony-creative-style", sonyStyle);
        params.set("creative-style", sonyStyle);
    }

    private static void mapToningAndSharpness(Camera.Parameters params, FujiRecipe recipe) {
        // Contrast: Combine highlight and shadow tones (-2..+4) -> (-3..+3)
        int avgTone = (recipe.highlightTone + recipe.shadowTone) / 2;
        int sonyContrast = clamp(avgTone, -3, 3);
        params.set("contrast", sonyContrast);
        params.set("sony-contrast", sonyContrast);

        // Saturation (Color): (-4..+4) -> (-3..+3)
        int sonySaturation = clamp((recipe.color * 3) / 4, -3, 3);
        params.set("saturation", sonySaturation);
        params.set("sony-saturation", sonySaturation);

        // Sharpness: (-4..+4) -> (-3..+3)
        int sonySharpness = clamp((recipe.sharpness * 3) / 4, -3, 3);
        params.set("sharpness", sonySharpness);
        params.set("sony-sharpness", sonySharpness);
    }

    private static void applySonyExModifiers(Object cameraEx, Camera.Parameters params, FujiRecipe recipe) {
        try {
            Class<?> camExClass = cameraEx.getClass();
            Method createModMethod = null;
            for (Method m : camExClass.getMethods()) {
                if ("createParametersModifier".equals(m.getName()) && m.getParameterTypes().length == 1) {
                    createModMethod = m;
                    break;
                }
            }
            if (createModMethod == null) return;

            Object modifier = createModMethod.invoke(cameraEx, params);
            if (modifier == null) return;

            Class<?> modClass = modifier.getClass();

            // 1. Dynamic Range (DR) -> Sony DRO (Dynamic Range Optimizer)
            // DR100 -> DRO Lv1 (Standard)
            // DR200 -> DRO Lv3 (Advanced +2)
            // DR400 -> DRO Lv5 (Advanced Max)
            int droLevel = 1;
            switch (recipe.dynamicRange) {
                case DR100: droLevel = 1; break;
                case DR200: droLevel = 3; break;
                case DR400: droLevel = 5; break;
            }

            // Must turn off Auto-HDR to prevent 3-shot sequence and 2s delays.
            params.set("sony-auto-hdr", "off");
            
            try {
                Method setDroMethod = modClass.getMethod("setDro", int.class);
                setDroMethod.invoke(modifier, droLevel);
            } catch (NoSuchMethodException ignored) {
                params.set("sony-dro", droLevel);
            }

            // 2. WB Fine Shift:
            // Fuji R (-9..+9) -> Sony Amber-Blue (A-B)
            // Fuji B (-9..+9) -> Sony Green-Magenta (G-M, inverted)
            int sonyAB = recipe.wbShiftR;
            int sonyGM = -recipe.wbShiftB;

            try {
                Method setWbShift = modClass.getMethod("setColorTempShift", int.class, int.class);
                setWbShift.invoke(modifier, sonyAB, sonyGM);
            } catch (NoSuchMethodException ignored) {
                params.set("sony-wb-shift-ab", sonyAB);
                params.set("sony-wb-shift-gm", sonyGM);
            }

        } catch (Throwable t) {
            Log.w(TAG, "CameraEx ParametersModifier not fully supported; skipping deep registers", t);
        }
    }

    private static int clamp(int val, int min, int max) {
        return Math.max(min, Math.min(max, val));
    }
}
