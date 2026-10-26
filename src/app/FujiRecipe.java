package com.fujilite.a7r2;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Fujifilm Recipe Engine model for Sony A7R II.
 * Holds 16 core parameters modeled after Fujifilm X-Processor recipes,
 * supports saving and recalling 7 custom presets (C1 - C7),
 * and computes 3x3 Color Correction Matrices and 1D Tone Curves.
 */
public class FujiRecipe {
    public static final int SLOT_COUNT = 7; // C1 - C7

    public enum FilmSim {
        PROVIA("PROVIA/STD"),
        VELVIA("VELVIA/VIVID"),
        ASTIA("ASTIA/SOFT"),
        CLASSIC_CHROME("CLASSIC CHR"),
        CLASSIC_NEG("CLASSIC NEG"),
        PRO_NEG_HI("PRO Neg. Hi"),
        PRO_NEG_STD("PRO Neg. Std"),
        ETERNA("ETERNA/CIN"),
        ACROS("ACROS (B&W)");

        public final String label;
        FilmSim(String l) { this.label = l; }
    }

    public enum DynamicRange {
        DR100("DR100"),
        DR200("DR200"),
        DR400("DR400");

        public final String label;
        DynamicRange(String l) { this.label = l; }
    }

    public enum EffectLevel {
        OFF("OFF"),
        WEAK("WEAK"),
        STRONG("STRONG");

        public final String label;
        EffectLevel(String l) { this.label = l; }
    }

    public enum WhiteBalance {
        AUTO("AUTO"),
        KELVIN("KELVIN"),
        DAYLIGHT("DAYLIGHT"),
        SHADE("SHADE");

        public final String label;
        WhiteBalance(String l) { this.label = l; }
    }

    // 16 Core Recipe Parameters
    public int recipeSlotIndex = 0;       // 0..6 (C1..C7)
    public FilmSim filmSim = FilmSim.CLASSIC_CHROME;
    public DynamicRange dynamicRange = DynamicRange.DR200;
    public EffectLevel grainEffect = EffectLevel.WEAK;
    public EffectLevel colorChrome = EffectLevel.STRONG;
    public EffectLevel colorChromeBlue = EffectLevel.WEAK;
    public WhiteBalance whiteBalance = WhiteBalance.KELVIN;
    public int colorTempK = 5600;         // 2500K .. 10000K
    public int wbShiftR = 2;              // -9 .. +9 (Red / Amber)
    public int wbShiftB = -3;             // -9 .. +9 (Blue)
    public int highlightTone = 1;         // -2 .. +4
    public int shadowTone = 2;            // -2 .. +4
    public int color = 1;                 // -4 .. +4 (Saturation)
    public int sharpness = 0;             // -4 .. +4
    public int noiseReduction = -2;       // -4 .. +4 (High ISO NR)
    public int clarity = 0;               // -4 .. +4

    private static final String PREF_NAME = "fuji_recipes_store";

    public FujiRecipe() {}

    public void saveToSlot(Context ctx, int slot) {
        if (ctx == null) return;
        this.recipeSlotIndex = slot;
        SharedPreferences pref = ctx.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
        SharedPreferences.Editor ed = pref.edit();
        String p = "c" + slot + "_";
        ed.putInt(p + "sim", filmSim.ordinal());
        ed.putInt(p + "dr", dynamicRange.ordinal());
        ed.putInt(p + "grain", grainEffect.ordinal());
        ed.putInt(p + "cc", colorChrome.ordinal());
        ed.putInt(p + "ccb", colorChromeBlue.ordinal());
        ed.putInt(p + "wb", whiteBalance.ordinal());
        ed.putInt(p + "k", colorTempK);
        ed.putInt(p + "wbr", wbShiftR);
        ed.putInt(p + "wbb", wbShiftB);
        ed.putInt(p + "hl", highlightTone);
        ed.putInt(p + "sh", shadowTone);
        ed.putInt(p + "col", color);
        ed.putInt(p + "sharp", sharpness);
        ed.putInt(p + "nr", noiseReduction);
        ed.putInt(p + "clarity", clarity);
        ed.commit();
    }

    public void loadFromSlot(Context ctx, int slot) {
        this.recipeSlotIndex = slot;
        if (ctx == null) return;
        SharedPreferences pref = ctx.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
        String p = "c" + slot + "_";
        if (!pref.contains(p + "sim")) {
            // Apply distinctive defaults for each slot if not configured yet
            applySlotDefault(slot);
            return;
        }
        filmSim = FilmSim.values()[pref.getInt(p + "sim", 0)];
        dynamicRange = DynamicRange.values()[pref.getInt(p + "dr", 0)];
        grainEffect = EffectLevel.values()[pref.getInt(p + "grain", 0)];
        colorChrome = EffectLevel.values()[pref.getInt(p + "cc", 0)];
        colorChromeBlue = EffectLevel.values()[pref.getInt(p + "ccb", 0)];
        whiteBalance = WhiteBalance.values()[pref.getInt(p + "wb", 0)];
        colorTempK = pref.getInt(p + "k", 5600);
        wbShiftR = pref.getInt(p + "wbr", 0);
        wbShiftB = pref.getInt(p + "wbb", 0);
        highlightTone = pref.getInt(p + "hl", 0);
        shadowTone = pref.getInt(p + "sh", 0);
        color = pref.getInt(p + "col", 0);
        sharpness = pref.getInt(p + "sharp", 0);
        noiseReduction = pref.getInt(p + "nr", 0);
        clarity = pref.getInt(p + "clarity", 0);
    }

    private void applySlotDefault(int slot) {
        switch (slot) {
            case 0: // C1: Classic Chrome Street
                filmSim = FilmSim.CLASSIC_CHROME;
                dynamicRange = DynamicRange.DR200;
                grainEffect = EffectLevel.WEAK;
                colorChrome = EffectLevel.STRONG;
                wbShiftR = 2; wbShiftB = -3;
                highlightTone = 1; shadowTone = 2;
                color = 1; sharpness = 0; noiseReduction = -2;
                break;
            case 1: // C2: Classic Neg Moody
                filmSim = FilmSim.CLASSIC_NEG;
                dynamicRange = DynamicRange.DR400;
                grainEffect = EffectLevel.STRONG;
                colorChrome = EffectLevel.STRONG;
                wbShiftR = 3; wbShiftB = -4;
                highlightTone = 2; shadowTone = 1;
                color = -1; sharpness = -1; noiseReduction = -3;
                break;
            case 2: // C3: Velvia Landscape
                filmSim = FilmSim.VELVIA;
                dynamicRange = DynamicRange.DR200;
                grainEffect = EffectLevel.OFF;
                colorChrome = EffectLevel.STRONG;
                wbShiftR = 1; wbShiftB = -1;
                highlightTone = 1; shadowTone = 2;
                color = 2; sharpness = 1; noiseReduction = -1;
                break;
            case 3: // C4: Astia Soft Portrait
                filmSim = FilmSim.ASTIA;
                dynamicRange = DynamicRange.DR200;
                grainEffect = EffectLevel.OFF;
                colorChrome = EffectLevel.WEAK;
                wbShiftR = 1; wbShiftB = 0;
                highlightTone = -1; shadowTone = 0;
                color = 1; sharpness = 0; noiseReduction = 0;
                break;
            case 4: // C5: Eterna Cinematic
                filmSim = FilmSim.ETERNA;
                dynamicRange = DynamicRange.DR400;
                grainEffect = EffectLevel.WEAK;
                colorChrome = EffectLevel.WEAK;
                wbShiftR = 2; wbShiftB = -2;
                highlightTone = -1; shadowTone = -1;
                color = -2; sharpness = -2; noiseReduction = 0;
                break;
            case 5: // C6: Acros B&W Noir
                filmSim = FilmSim.ACROS;
                dynamicRange = DynamicRange.DR200;
                grainEffect = EffectLevel.STRONG;
                colorChrome = EffectLevel.OFF;
                wbShiftR = 0; wbShiftB = 0;
                highlightTone = 2; shadowTone = 2;
                color = 0; sharpness = 1; noiseReduction = -4;
                break;
            case 6: // C7: Provia Everyday Standard
            default:
                filmSim = FilmSim.PROVIA;
                dynamicRange = DynamicRange.DR100;
                grainEffect = EffectLevel.OFF;
                colorChrome = EffectLevel.OFF;
                wbShiftR = 0; wbShiftB = 0;
                highlightTone = 0; shadowTone = 0;
                color = 0; sharpness = 0; noiseReduction = 0;
                break;
        }
    }

    /**
     * Computes the 3x3 Color Correction Matrix in Q8.8 fixed-point (256 = 1.0).
     * Incorporates base film simulation, saturation scaling, and WB shift tint.
     */
    public short[] computeCcmQ8() {
        short[] ccm = new short[9];

        // 1. Base film matrix in Q8.8
        int r00, r01, r02;
        int r10, r11, r12;
        int r20, r21, r22;

        switch (filmSim) {
            case VELVIA:
                r00 = 290; r01 = -20; r02 = -14;
                r10 = -16; r11 = 295; r12 = -23;
                r20 = -10; r21 = -25; r22 = 291;
                break;
            case ASTIA:
                r00 = 248; r01 = 6;   r02 = 2;
                r10 = 2;   r11 = 254; r12 = 0;
                r20 = 0;   r21 = 4;   r22 = 252;
                break;
            case CLASSIC_CHROME:
                r00 = 245; r01 = 8;   r02 = 3;
                r10 = 2;   r11 = 250; r12 = 4;
                r20 = 6;   r21 = 8;   r22 = 242;
                break;
            case CLASSIC_NEG:
                r00 = 230; r01 = 16;  r02 = 10;
                r10 = -12; r11 = 270; r12 = -2;
                r20 = 10;  r21 = -15; r22 = 261;
                break;
            case PRO_NEG_HI:
                r00 = 256; r01 = 4;   r02 = -4;
                r10 = -2;  r11 = 258; r12 = 0;
                r20 = 2;   r21 = -2;  r22 = 256;
                break;
            case PRO_NEG_STD:
                r00 = 250; r01 = 3;   r02 = 3;
                r10 = 3;   r11 = 252; r12 = 1;
                r20 = 1;   r21 = 2;   r22 = 253;
                break;
            case ETERNA:
                r00 = 210; r01 = 25;  r02 = 21;
                r10 = 20;  r11 = 215; r12 = 21;
                r20 = 20;  r21 = 24;  r22 = 212;
                break;
            case ACROS:
                // Grayscale luminance coefficients (ITU-R BT.601) in Q8.8
                // R: 0.299 * 256 = 77, G: 0.587 * 256 = 150, B: 0.114 * 256 = 29
                r00 = 77;  r01 = 150; r02 = 29;
                r10 = 77;  r11 = 150; r12 = 29;
                r20 = 77;  r21 = 150; r22 = 29;
                break;
            case PROVIA:
            default:
                r00 = 266; r01 = -5;  r02 = -5;
                r10 = -5;  r11 = 266; r12 = -5;
                r20 = -5;  r21 = -5;  r22 = 266;
                break;
        }

        // 2. Adjust for Color (Saturation) parameter (-4 to +4)
        if (filmSim != FilmSim.ACROS && color != 0) {
            // Factor: 1.0 + (color * 0.08)
            int satMul = 256 + (color * 20); // Q8.8
            r00 = (r00 * satMul) >> 8;
            r11 = (r11 * satMul) >> 8;
            r22 = (r22 * satMul) >> 8;
        }

        // 3. Adjust for WB Shift (R and B)
        // wbShiftR scales Red row, wbShiftB scales Blue row
        if (wbShiftR != 0) {
            int rMul = 256 + (wbShiftR * 8);
            r00 = (r00 * rMul) >> 8;
            r01 = (r01 * rMul) >> 8;
            r02 = (r02 * rMul) >> 8;
        }
        if (wbShiftB != 0) {
            int bMul = 256 + (wbShiftB * 8);
            r20 = (r20 * bMul) >> 8;
            r21 = (r21 * bMul) >> 8;
            r22 = (r22 * bMul) >> 8;
        }

        ccm[0] = (short) r00; ccm[1] = (short) r01; ccm[2] = (short) r02;
        ccm[3] = (short) r10; ccm[4] = (short) r11; ccm[5] = (short) r12;
        ccm[6] = (short) r20; ccm[7] = (short) r21; ccm[8] = (short) r22;
        return ccm;
    }

    /**
     * Generates a 256-byte 1D Tone Curve lookup table for a specific channel.
     * Incorporates Highlight Tone (-2..+4), Shadow Tone (-2..+4), and Film simulation characteristics.
     */
    public byte[] generateToneCurve(char channel) {
        byte[] curve = new byte[256];

        // Base contrast adjustment: S-curve
        int contrastBoost = (highlightTone + shadowTone) * 8;
        int shadowLift = -shadowTone * 6;      // Positive shadowTone darkens shadows
        int highlightRoll = highlightTone * 6; // Positive highlightTone boosts highlights

        // Film-specific adjustments
        if (filmSim == FilmSim.VELVIA) {
            contrastBoost += 25;
            shadowLift -= 15;
        } else if (filmSim == FilmSim.ETERNA) {
            contrastBoost -= 20;
            shadowLift += 20;
            highlightRoll += 15;
        } else if (filmSim == FilmSim.CLASSIC_NEG) {
            contrastBoost += 15;
            shadowLift += 10;
        }

        double contrastFactor = 1.0 + ((double) contrastBoost / 100.0);

        // Fast algebraic sigmoid: eliminates 768 expensive Math.exp() calls on Dalvik ARM
        for (int i = 0; i < 256; i++) {
            double x = i / 255.0;
            double centered = (x - 0.5) * contrastFactor;
            double z = centered * 3.0;
            double s = 0.5 + 0.5 * (z / (1.0 + Math.abs(z)));

            // Shadow adjustment below 0.35
            if (x < 0.35) {
                s += ((double) shadowLift / 255.0) * ((0.35 - x) / 0.35);
            }
            // Highlight adjustment above 0.65
            if (x > 0.65) {
                s += ((double) highlightRoll / 255.0) * ((x - 0.65) / 0.35);
            }

            if (s < 0.0) s = 0.0;
            if (s > 1.0) s = 1.0;

            int val = (int) (s * 255.0 + 0.5);
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            curve[i] = (byte) val;
        }

        return curve;
    }
}
