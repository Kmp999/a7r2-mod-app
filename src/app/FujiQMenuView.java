package com.fujilite.a7r2;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.KeyEvent;
import android.view.View;

/**
 * Fujifilm Q-Menu 4x4 Grid view for Sony A7R II.
 * Rendered at 640x480 (VGA) resolution with Zero-GC allocation during onDraw().
 * Handled entirely with physical hardware buttons and dials.
 */
public class FujiQMenuView extends View {

    public interface OnRecipeChangeListener {
        void onRecipeChanged(FujiRecipe recipe);
        void onDismissRequest();
    }

    private static final int COLS = 4;
    private static final int ROWS = 4;
    private static final int TOTAL_CELLS = 16;

    // Fujifilm aesthetic color palette
    private static final int COLOR_BG_DIM        = 0xCC000000; // Semi-transparent black (80%)
    private static final int COLOR_CELL_NORMAL   = 0x33222222;
    private static final int COLOR_CELL_FOCUSED  = 0x99333333;
    private static final int COLOR_AMBER_GOLD    = 0xFFFFAA00; // Fujifilm Signature Amber
    private static final int COLOR_TEXT_LABEL    = 0xFF888888;
    private static final int COLOR_TEXT_VALUE    = 0xFFFFFFFF;

    private int selectedIndex = 0; // 0..15
    private FujiRecipe recipe;
    private OnRecipeChangeListener listener;

    // Pre-allocated graphic objects (Zero GC during onDraw)
    private final Paint bgPaint = new Paint();
    private final Paint cellBorderPaint = new Paint();
    private final Paint cellFillPaint = new Paint();
    private final Paint textLabelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textValuePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint headerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final RectF cellRect = new RectF();
    private final String[] itemTitles = new String[TOTAL_CELLS];
    private final String[] cachedValues = new String[TOTAL_CELLS];

    public FujiQMenuView(Context context, FujiRecipe recipe, OnRecipeChangeListener listener) {
        super(context);
        this.recipe = recipe;
        this.listener = listener;
        setFocusable(true);
        setFocusableInTouchMode(true);
        init();
    }

    public void setRecipe(FujiRecipe recipe) {
        this.recipe = recipe;
        refreshCachedValues();
        invalidate();
    }

    private void init() {
        setWillNotDraw(false);

        bgPaint.setColor(COLOR_BG_DIM);

        cellBorderPaint.setStyle(Paint.Style.STROKE);
        cellBorderPaint.setStrokeWidth(2.5f);

        cellFillPaint.setStyle(Paint.Style.FILL);

        Typeface mono = Typeface.MONOSPACE;
        textLabelPaint.setTypeface(mono);
        textLabelPaint.setTextSize(12f);
        textLabelPaint.setTextAlign(Paint.Align.LEFT);

        textValuePaint.setTypeface(Typeface.create(mono, Typeface.BOLD));
        textValuePaint.setTextSize(15f);
        textValuePaint.setTextAlign(Paint.Align.CENTER);

        headerPaint.setTypeface(Typeface.create(mono, Typeface.BOLD));
        headerPaint.setTextSize(13f);
        headerPaint.setColor(COLOR_AMBER_GOLD);

        // 16 Fujifilm Q-Menu items
        itemTitles[0]  = "BASE FILM";
        itemTitles[1]  = "D-RANGE";
        itemTitles[2]  = "GRAIN";
        itemTitles[3]  = "CHROME";
        itemTitles[4]  = "FX BLUE";
        itemTitles[5]  = "WB MODE";
        itemTitles[6]  = "WB SHIFT R";
        itemTitles[7]  = "WB SHIFT B";
        itemTitles[8]  = "H-TONE";
        itemTitles[9]  = "S-TONE";
        itemTitles[10] = "COLOR";
        itemTitles[11] = "SHARP";
        itemTitles[12] = "NR";
        itemTitles[13] = "CLARITY";
        itemTitles[14] = "RECIPE";
        itemTitles[15] = "SLOT SAVE";

        refreshCachedValues();
    }

    public void refreshCachedValues() {
        if (recipe == null) return;
        cachedValues[0]  = recipe.filmSim.label;
        cachedValues[1]  = recipe.dynamicRange.label;
        cachedValues[2]  = recipe.grainEffect.label;
        cachedValues[3]  = recipe.colorChrome.label;
        cachedValues[4]  = recipe.colorChromeBlue.label;
        cachedValues[5]  = recipe.whiteBalance.label;
        cachedValues[6]  = (recipe.wbShiftR > 0 ? "+" : "") + recipe.wbShiftR;
        cachedValues[7]  = (recipe.wbShiftB > 0 ? "+" : "") + recipe.wbShiftB;
        cachedValues[8]  = (recipe.highlightTone > 0 ? "+" : "") + recipe.highlightTone;
        cachedValues[9]  = (recipe.shadowTone > 0 ? "+" : "") + recipe.shadowTone;
        cachedValues[10] = (recipe.color > 0 ? "+" : "") + recipe.color;
        cachedValues[11] = (recipe.sharpness > 0 ? "+" : "") + recipe.sharpness;
        cachedValues[12] = (recipe.noiseReduction > 0 ? "+" : "") + recipe.noiseReduction;
        cachedValues[13] = (recipe.clarity > 0 ? "+" : "") + recipe.clarity;
        cachedValues[14] = "C" + (recipe.recipeSlotIndex + 1);
        cachedValues[15] = "SAVE C" + (recipe.recipeSlotIndex + 1);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        final int w = getWidth();
        final int h = getHeight();
        if (w <= 0 || h <= 0) return;

        // 1. Semi-transparent black background
        canvas.drawRect(0, 0, w, h, bgPaint);

        // 2. Header Status
        canvas.drawText("FUJIFILM Q-MENU [DIAL: ADJUST | SHUTTER / TRASH: EXIT]", 16, 26, headerPaint);

        // 3. 4x4 Grid layout calculation
        final float marginX = 14f;
        final float marginTop = 36f;
        final float marginBottom = 14f;
        final float gridW = w - (marginX * 2);
        final float gridH = h - marginTop - marginBottom;

        final float cellW = gridW / COLS;
        final float cellH = gridH / ROWS;
        final float pad = 3f;

        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                int index = (r * COLS) + c;
                boolean isFocused = (index == selectedIndex);

                float left = marginX + (c * cellW) + pad;
                float top = marginTop + (r * cellH) + pad;
                float right = marginX + ((c + 1) * cellW) - pad;
                float bottom = marginTop + ((r + 1) * cellH) - pad;

                cellRect.set(left, top, right, bottom);

                // Cell fill
                cellFillPaint.setColor(isFocused ? COLOR_CELL_FOCUSED : COLOR_CELL_NORMAL);
                canvas.drawRect(cellRect, cellFillPaint);

                // Cell border
                if (isFocused) {
                    cellBorderPaint.setColor(COLOR_AMBER_GOLD);
                    cellBorderPaint.setStrokeWidth(2.5f);
                } else {
                    cellBorderPaint.setColor(0x44FFFFFF);
                    cellBorderPaint.setStrokeWidth(1.0f);
                }
                canvas.drawRect(cellRect, cellBorderPaint);

                // Label
                textLabelPaint.setColor(isFocused ? COLOR_AMBER_GOLD : COLOR_TEXT_LABEL);
                canvas.drawText(itemTitles[index], left + 6f, top + 15f, textLabelPaint);

                // Value
                textValuePaint.setColor(isFocused ? COLOR_TEXT_VALUE : 0xFFCCCCCC);
                float centerX = (left + right) / 2.0f;
                float centerY = top + (cellH * 0.68f);
                if (cachedValues[index] != null) {
                    canvas.drawText(cachedValues[index], centerX, centerY, textValuePaint);
                }
            }
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        switch (keyCode) {
            // D-Pad Navigation
            case KeyEvent.KEYCODE_DPAD_UP:
                selectedIndex = (selectedIndex - COLS + TOTAL_CELLS) % TOTAL_CELLS;
                invalidate();
                return true;

            case KeyEvent.KEYCODE_DPAD_DOWN:
                selectedIndex = (selectedIndex + COLS) % TOTAL_CELLS;
                invalidate();
                return true;

            case KeyEvent.KEYCODE_DPAD_LEFT:
                selectedIndex = (selectedIndex % COLS == 0) ? selectedIndex + COLS - 1 : selectedIndex - 1;
                invalidate();
                return true;

            case KeyEvent.KEYCODE_DPAD_RIGHT:
                selectedIndex = ((selectedIndex + 1) % COLS == 0) ? selectedIndex - COLS + 1 : selectedIndex + 1;
                invalidate();
                return true;

            // Front Dial: Fast 2-step adjustment
            case SonyKeyCodes.SONY_KEY_DIAL1_CW:
                adjustCurrentSetting(2);
                return true;

            case SonyKeyCodes.SONY_KEY_DIAL1_CCW:
                adjustCurrentSetting(-2);
                return true;

            // Rear Dial & Control Wheel: Precise 1-step adjustment
            case SonyKeyCodes.SONY_KEY_DIAL2_CW:
            case SonyKeyCodes.SONY_KEY_DIAL3_CW:
            case KeyEvent.KEYCODE_PAGE_UP:
                adjustCurrentSetting(1);
                return true;

            case SonyKeyCodes.SONY_KEY_DIAL2_CCW:
            case SonyKeyCodes.SONY_KEY_DIAL3_CCW:
            case KeyEvent.KEYCODE_PAGE_DOWN:
                adjustCurrentSetting(-1);
                return true;

            // Center Enter button: Reset selected cell to factory default (0 or Standard)
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_ENTER:
                if (selectedIndex == 15) {
                    recipe.saveToSlot(getContext(), recipe.recipeSlotIndex);
                    refreshCachedValues();
                    invalidate();
                } else {
                    resetCurrentSetting();
                }
                return true;

            // Fn key in Q-Menu: Instant Recipe Save
            case SonyKeyCodes.SONY_KEY_FN:
                recipe.saveToSlot(getContext(), recipe.recipeSlotIndex);
                refreshCachedValues();
                invalidate();
                return true;

            // Half-shutter, Trash, or Menu: Immediate Dismiss
            case SonyKeyCodes.SONY_KEY_SHUTTER_HALF:
            case SonyKeyCodes.ANDROID_CAMERA:
            case SonyKeyCodes.SONY_KEY_C3_TRASH:
            case SonyKeyCodes.ANDROID_DEL:
            case SonyKeyCodes.ANDROID_CLEAR:
            case KeyEvent.KEYCODE_MENU:
                if (listener != null) {
                    listener.onDismissRequest();
                }
                return true;
        }

        return super.onKeyDown(keyCode, event);
    }

    public void resetCurrentSetting() {
        if (recipe == null) return;

        switch (selectedIndex) {
            case 0:  recipe.filmSim = FujiRecipe.FilmSim.PROVIA; break;
            case 1:  recipe.dynamicRange = FujiRecipe.DynamicRange.DR100; break;
            case 2:  recipe.grainEffect = FujiRecipe.EffectLevel.OFF; break;
            case 3:  recipe.colorChrome = FujiRecipe.EffectLevel.OFF; break;
            case 4:  recipe.colorChromeBlue = FujiRecipe.EffectLevel.OFF; break;
            case 5:  recipe.whiteBalance = FujiRecipe.WhiteBalance.AUTO; break;
            case 6:  recipe.wbShiftR = 0; break;
            case 7:  recipe.wbShiftB = 0; break;
            case 8:  recipe.highlightTone = 0; break;
            case 9:  recipe.shadowTone = 0; break;
            case 10: recipe.color = 0; break;
            case 11: recipe.sharpness = 0; break;
            case 12: recipe.noiseReduction = 0; break;
            case 13: recipe.clarity = 0; break;
            case 14: recipe.loadFromSlot(getContext(), 0); break;
            case 15: recipe.saveToSlot(getContext(), recipe.recipeSlotIndex); break;
        }

        refreshCachedValues();
        invalidate();

        if (listener != null) {
            listener.onRecipeChanged(recipe);
        }
    }

    private void adjustCurrentSetting(int delta) {
        if (recipe == null) return;

        switch (selectedIndex) {
            case 0: { // Base Film
                int len = FujiRecipe.FilmSim.values().length;
                int next = (recipe.filmSim.ordinal() + delta + len) % len;
                recipe.filmSim = FujiRecipe.FilmSim.values()[next];
                break;
            }
            case 1: { // Dynamic Range
                int len = FujiRecipe.DynamicRange.values().length;
                int next = (recipe.dynamicRange.ordinal() + delta + len) % len;
                recipe.dynamicRange = FujiRecipe.DynamicRange.values()[next];
                break;
            }
            case 2: { // Grain
                int len = FujiRecipe.EffectLevel.values().length;
                int next = (recipe.grainEffect.ordinal() + delta + len) % len;
                recipe.grainEffect = FujiRecipe.EffectLevel.values()[next];
                break;
            }
            case 3: { // Color Chrome
                int len = FujiRecipe.EffectLevel.values().length;
                int next = (recipe.colorChrome.ordinal() + delta + len) % len;
                recipe.colorChrome = FujiRecipe.EffectLevel.values()[next];
                break;
            }
            case 4: { // FX Blue
                int len = FujiRecipe.EffectLevel.values().length;
                int next = (recipe.colorChromeBlue.ordinal() + delta + len) % len;
                recipe.colorChromeBlue = FujiRecipe.EffectLevel.values()[next];
                break;
            }
            case 5: { // WB Mode
                int len = FujiRecipe.WhiteBalance.values().length;
                int next = (recipe.whiteBalance.ordinal() + delta + len) % len;
                recipe.whiteBalance = FujiRecipe.WhiteBalance.values()[next];
                break;
            }
            case 6: // WB Shift R (-9..+9)
                recipe.wbShiftR = Math.max(-9, Math.min(9, recipe.wbShiftR + delta));
                break;

            case 7: // WB Shift B (-9..+9)
                recipe.wbShiftB = Math.max(-9, Math.min(9, recipe.wbShiftB + delta));
                break;

            case 8: // Highlight Tone (-2..+4)
                recipe.highlightTone = Math.max(-2, Math.min(4, recipe.highlightTone + delta));
                break;

            case 9: // Shadow Tone (-2..+4)
                recipe.shadowTone = Math.max(-2, Math.min(4, recipe.shadowTone + delta));
                break;

            case 10: // Color (-4..+4)
                recipe.color = Math.max(-4, Math.min(4, recipe.color + delta));
                break;

            case 11: // Sharpness (-4..+4)
                recipe.sharpness = Math.max(-4, Math.min(4, recipe.sharpness + delta));
                break;

            case 12: // Noise Reduction (-4..+4)
                recipe.noiseReduction = Math.max(-4, Math.min(4, recipe.noiseReduction + delta));
                break;

            case 13: // Clarity (-4..+4)
                recipe.clarity = Math.max(-4, Math.min(4, recipe.clarity + delta));
                break;

            case 14: { // Recipe Slot C1..C7
                int nextSlot = (recipe.recipeSlotIndex + delta + FujiRecipe.SLOT_COUNT) % FujiRecipe.SLOT_COUNT;
                recipe.loadFromSlot(getContext(), nextSlot);
                break;
            }
            case 15: { // Slot save
                recipe.saveToSlot(getContext(), recipe.recipeSlotIndex);
                break;
            }
        }

        refreshCachedValues();
        invalidate();

        if (listener != null) {
            listener.onRecipeChanged(recipe);
        }
    }
}
