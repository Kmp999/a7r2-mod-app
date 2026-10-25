package com.fujilite.a7r2;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.KeyEvent;
import android.view.View;

/**
 * Fujifilm Q-Menu 4x4 Grid & Unified Live View HUD for Sony A7R II.
 *
 * Rendered at 640x480 (VGA) resolution with Zero-GC allocation during onDraw().
 * Fully controlled via physical dials and hardware buttons.
 *
 * Acts as the single high-performance overlay over SurfaceView:
 *  - When Q-Menu is closed: Transparent Live View HUD (Top status, Bottom exposure telemetry, Key Debugger).
 *  - When Q-Menu is open: Semi-transparent 4x4 Fujifilm grid with Amber cursor.
 *  - When reviewing a photo: Quick Review photo display overlay.
 *
 * Focusable is set to false to prevent stealing Android focus.
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
    private static final int COLOR_BAR_BG        = 0x77000000; // Subtle bar backing for Live View HUD
    private static final int COLOR_CELL_NORMAL   = 0x33222222;
    private static final int COLOR_CELL_FOCUSED  = 0x99333333;
    private static final int COLOR_AMBER_GOLD    = 0xFFFFAA00; // Fujifilm Signature Amber
    private static final int COLOR_TEXT_LABEL    = 0xFF888888;
    private static final int COLOR_TEXT_VALUE    = 0xFFFFFFFF;
    private static final int COLOR_WHITE         = 0xFFFFFFFF;

    private int selectedIndex = 0; // 0..15
    private FujiRecipe recipe;
    private OnRecipeChangeListener listener;

    private boolean isQMenuOpen = false;
    private Bitmap reviewBitmap = null;
    private String reviewInfo = null;

    // Live View Telemetry State
    private String hudShutter = "1/125";
    private String hudAperture = "F2.8";
    private String hudIso = "ISO 400";
    private String hudEv = "+0.0";
    private String hudAf = "AF-S";
    private boolean hudAel = false;
    private String topStatus = "FUJIFILM LIVE";
    private String carouselBanner = null;
    private long carouselBannerEndTime = 0;

    // Zero-GC Live View Canvas Buffer
    private Bitmap liveViewBitmap = null;
    private java.nio.ByteBuffer liveViewDirectBuffer = null;

    // Pre-allocated graphic objects (Zero GC during onDraw)
    private final Paint bgPaint = new Paint();
    private final Paint barBgPaint = new Paint();
    private final Paint cellBorderPaint = new Paint();
    private final Paint cellFillPaint = new Paint();
    private final Paint textLabelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textValuePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint headerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hudPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint debugPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bannerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bannerTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final RectF cellRect = new RectF();
    private final Rect srcRect = new Rect();
    private final Rect dstRect = new Rect();
    private final String[] itemTitles = new String[TOTAL_CELLS];
    private final String[] cachedValues = new String[TOTAL_CELLS];

    public FujiQMenuView(Context context, FujiRecipe recipe, OnRecipeChangeListener listener) {
        super(context);
        this.recipe = recipe;
        this.listener = listener;

        // Never steal focus from Activity dispatchKeyEvent
        setFocusable(false);
        setFocusableInTouchMode(false);
        init();
    }

    private int splitScreenMode = 0; // 0=Full LUT, 1=Split 50/50, 2=Bypass
    private int highlightedSegment = 0; // 1=SS, 2=Aperture, 3=ISO, 4=EV, 5=FilmSim
    private long highlightEndTime = 0;

    public void setSplitScreenMode(int mode) {
        this.splitScreenMode = mode;
        postInvalidate();
    }

    public int getSplitScreenMode() {
        return splitScreenMode;
    }

    public void cycleSplitScreenMode() {
        splitScreenMode = (splitScreenMode + 1) % 3;
        String modeName = (splitScreenMode == 0) ? "LIVE LUT [FULL]" : ((splitScreenMode == 1) ? "SPLIT 50/50 [BEFORE/AFTER]" : "BYPASS [SONY]");
        showCarouselBanner(modeName);
        postInvalidate();
    }

    public void highlightSegment(int segment) {
        this.highlightedSegment = segment;
        this.highlightEndTime = System.currentTimeMillis() + 1500;
        postInvalidate();
    }

    public void processLiveViewFrame(byte[] nv21, int width, int height, int grainStrength) {
        if (liveViewBitmap == null || liveViewBitmap.getWidth() != width || liveViewBitmap.getHeight() != height) {
            liveViewBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.RGB_565);
            liveViewDirectBuffer = java.nio.ByteBuffer.allocateDirect(width * height * 2);
        }

        boolean success = ColorEngine.gradePreviewNV21Safe(nv21, liveViewDirectBuffer, width, height, splitScreenMode, grainStrength);
        if (success) {
            liveViewDirectBuffer.rewind();
            liveViewBitmap.copyPixelsFromBuffer(liveViewDirectBuffer);
            postInvalidate();
        }
    }

    public void clearLiveViewBuffer() {
        liveViewBitmap = null;
        liveViewDirectBuffer = null;
        postInvalidate();
    }

    public void showCarouselBanner(String text) {
        carouselBanner = text;
        carouselBannerEndTime = System.currentTimeMillis() + 1500; // Show for 1.5 seconds
        postInvalidate();
    }

    public void setRecipe(FujiRecipe recipe) {
        this.recipe = recipe;
        refreshCachedValues();
        updateTopStatus();
        postInvalidate();
    }

    public boolean isQMenuOpen() {
        return isQMenuOpen;
    }

    public void setQMenuOpen(boolean open) {
        this.isQMenuOpen = open;
        if (open) {
            refreshCachedValues();
        }
        postInvalidate();
    }

    public void setQuickReviewBitmap(Bitmap bmp, String info) {
        this.reviewBitmap = bmp;
        this.reviewInfo = info;
        postInvalidate();
    }

    public void clearQuickReview() {
        this.reviewBitmap = null;
        this.reviewInfo = null;
        postInvalidate();
    }

    public boolean isReviewing() {
        return (reviewBitmap != null);
    }

    public void updateTelemetry(String shutter, String aperture, String iso, String ev, boolean ael, String afMode) {
        this.hudShutter = shutter;
        this.hudAperture = aperture;
        this.hudIso = iso;
        this.hudEv = ev;
        this.hudAel = ael;
        this.hudAf = afMode;
        postInvalidate();
    }

    private void updateTopStatus() {
        if (recipe != null) {
            topStatus = recipe.filmSim.label + "  |  " + recipe.dynamicRange.label + "  |  C" + (recipe.recipeSlotIndex + 1);
        }
    }

    private void init() {
        setWillNotDraw(false);

        bgPaint.setColor(COLOR_BG_DIM);
        barBgPaint.setColor(COLOR_BAR_BG);

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
        headerPaint.setTextSize(14f);
        headerPaint.setColor(COLOR_AMBER_GOLD);

        hudPaint.setTypeface(Typeface.create(mono, Typeface.BOLD));
        hudPaint.setTextSize(15f);
        hudPaint.setColor(COLOR_AMBER_GOLD);

        debugPaint.setTypeface(mono);
        debugPaint.setTextSize(11f);
        debugPaint.setColor(0xFFAAAAAA);
        debugPaint.setTextAlign(Paint.Align.RIGHT);

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
        updateTopStatus();
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

        // --------------------------------------------------------------------
        // Mode 1: Quick Photo Review Display
        // --------------------------------------------------------------------
        if (reviewBitmap != null) {
            srcRect.set(0, 0, reviewBitmap.getWidth(), reviewBitmap.getHeight());
            dstRect.set(0, 0, w, h);
            canvas.drawBitmap(reviewBitmap, srcRect, dstRect, null);

            canvas.drawRect(0, h - 36, w, h, barBgPaint);
            canvas.drawText(reviewInfo != null ? reviewInfo : "PREVIEW", 16, h - 14, headerPaint);
            canvas.drawText("PRESS PLAY / SHUTTER TO EXIT", w - 16, h - 14, debugPaint);
            return;
        }

        // --------------------------------------------------------------------
        // Mode 2: 4x4 Fujifilm Q-Menu Grid
        // --------------------------------------------------------------------
        if (isQMenuOpen) {
            // Semi-transparent black background
            canvas.drawRect(0, 0, w, h, bgPaint);

            // Header Status
            canvas.drawText("FUJIFILM Q-MENU [FRONT/REAR DIAL: ADJUST | TRASH: EXIT]", 16, 24, headerPaint);

            // 4x4 Grid layout calculation
            final float marginX = 14f;
            final float marginTop = 32f;
            final float marginBottom = 34f;
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

                    // Cell border (Amber Gold focus)
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
                    canvas.drawText(itemTitles[index], left + 6f, top + 14f, textLabelPaint);

                    // Value
                    textValuePaint.setColor(isFocused ? COLOR_TEXT_VALUE : 0xFFCCCCCC);
                    float centerX = (left + right) / 2.0f;
                    float centerY = top + (cellH * 0.68f);
                    if (cachedValues[index] != null) {
                        canvas.drawText(cachedValues[index], centerX, centerY, textValuePaint);
                    }
                }
            }
        } else {
            // ----------------------------------------------------------------
            // Mode 3: Live View Viewfinder (Real-Time LUT Canvas Display)
            // ----------------------------------------------------------------
            if (liveViewBitmap != null) {
                dstRect.set(0, 0, w, h);
                canvas.drawBitmap(liveViewBitmap, null, dstRect, null);
            }

            // Top Bar
            canvas.drawRect(0, 0, w, 28, barBgPaint);
            canvas.drawText(topStatus, 16, 20, headerPaint);

            if (splitScreenMode == 1) {
                canvas.drawText("SONY [ORIGINAL]", (w / 4f) - 30f, 20, debugPaint);
                canvas.drawText("[FUJI 3D-LUT]", (w * 3f / 4f) - 30f, 20, headerPaint);
            } else if (splitScreenMode == 2) {
                canvas.drawText("[BYPASS]", w - 180, 20, debugPaint);
            }

            if (SonyKeyCodes.sDebugKeyLoggingEnabled) {
                canvas.drawText("[" + SonyKeyCodes.sLastKeyName + "]", w - 16, 20, debugPaint);
            }
        }

        // --------------------------------------------------------------------
        // Carousel Banner (Film Sim Notification overlay)
        // --------------------------------------------------------------------
        if (carouselBanner != null && System.currentTimeMillis() < carouselBannerEndTime) {
            float bannerTop = (h / 2f) - 22f;
            float bannerBottom = (h / 2f) + 22f;

            bannerPaint.setColor(0xDD000000);
            canvas.drawRect(0, bannerTop, w, bannerBottom, bannerPaint);

            bannerTextPaint.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
            bannerTextPaint.setTextSize(18f);
            bannerTextPaint.setColor(COLOR_AMBER_GOLD);
            bannerTextPaint.setTextAlign(Paint.Align.CENTER);
            
            canvas.drawText("[ " + carouselBanner + " ]", w / 2f, bannerTop + 28f, bannerTextPaint);
        }

        // --------------------------------------------------------------------
        // Common: Bottom Exposure Telemetry Bar (Zero-GC with Parameter Highlighting)
        // --------------------------------------------------------------------
        canvas.drawRect(0, h - 28, w, h, barBgPaint);

        boolean isHighlighted = (System.currentTimeMillis() < highlightEndTime);
        float xPos = 16f;

        // 1. Shutter Speed (Segment 1)
        hudPaint.setColor((isHighlighted && highlightedSegment == 1) ? COLOR_AMBER_GOLD : 0xFFFFFFFF);
        canvas.drawText(hudShutter, xPos, h - 9, hudPaint);
        xPos += hudPaint.measureText(hudShutter) + 10f;

        hudPaint.setColor(COLOR_TEXT_LABEL);
        canvas.drawText("|", xPos, h - 9, hudPaint);
        xPos += 10f;

        // 2. Aperture F-Stop (Segment 2)
        hudPaint.setColor((isHighlighted && highlightedSegment == 2) ? COLOR_AMBER_GOLD : 0xFFFFFFFF);
        canvas.drawText(hudAperture, xPos, h - 9, hudPaint);
        xPos += hudPaint.measureText(hudAperture) + 10f;

        hudPaint.setColor(COLOR_TEXT_LABEL);
        canvas.drawText("|", xPos, h - 9, hudPaint);
        xPos += 10f;

        // 3. ISO (Segment 3)
        hudPaint.setColor((isHighlighted && highlightedSegment == 3) ? COLOR_AMBER_GOLD : 0xFFFFFFFF);
        canvas.drawText(hudIso, xPos, h - 9, hudPaint);
        xPos += hudPaint.measureText(hudIso) + 10f;

        hudPaint.setColor(COLOR_TEXT_LABEL);
        canvas.drawText("|", xPos, h - 9, hudPaint);
        xPos += 10f;

        // 4. EV (Segment 4)
        hudPaint.setColor((isHighlighted && highlightedSegment == 4) ? COLOR_AMBER_GOLD : 0xFFFFFFFF);
        canvas.drawText(hudEv + " EV", xPos, h - 9, hudPaint);
        xPos += hudPaint.measureText(hudEv + " EV") + 10f;

        hudPaint.setColor(COLOR_TEXT_LABEL);
        canvas.drawText("|", xPos, h - 9, hudPaint);
        xPos += 10f;

        // AF Mode & AE-L
        hudPaint.setColor(0xFFFFFFFF);
        canvas.drawText(hudAf, xPos, h - 9, hudPaint);
        xPos += hudPaint.measureText(hudAf) + 8f;

        if (hudAel) {
            hudPaint.setColor(COLOR_AMBER_GOLD);
            canvas.drawText("[AE-L]", xPos, h - 9, hudPaint);
        }

        // 5. Film Sim (Segment 5) on far right
        if (recipe != null) {
            String simName = recipe.filmSim.label;
            hudPaint.setColor((isHighlighted && highlightedSegment == 5) ? COLOR_AMBER_GOLD : COLOR_TEXT_LABEL);
            canvas.drawText(simName, w - hudPaint.measureText(simName) - 16f, h - 9, hudPaint);
        }
    }

    // ------------------------------------------------------------------------
    // Q-Menu Navigation & Adjustment Helpers
    // ------------------------------------------------------------------------

    public void moveSelection(int dCol, int dRow) {
        if (!isQMenuOpen) return;
        int col = selectedIndex % COLS;
        int row = selectedIndex / COLS;

        col = (col + dCol + COLS) % COLS;
        row = (row + dRow + ROWS) % ROWS;

        selectedIndex = row * COLS + col;
        postInvalidate();
    }

    public void adjustCurrentSetting(int delta) {
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
        updateTopStatus();
        postInvalidate();

        if (listener != null) {
            listener.onRecipeChanged(recipe);
        }
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
        updateTopStatus();
        postInvalidate();

        if (listener != null) {
            listener.onRecipeChanged(recipe);
        }
    }
}
