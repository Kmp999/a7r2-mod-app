package com.fujilite.a7r2;

public class AppState {
    public enum Mode {
        INTERNAL_PIPELINE,
        JPEG_PROXY,
        PREVIEW_ONLY,
        ASYNC_FULLRES
    }

    // Film simulations matching FujiRecipe
    public static final String[] PRESET_NAMES = {
        "Provia", "Velvia", "Astia", "Classic Chr", "Classic Neg",
        "Pro Neg Hi", "Pro Neg Std", "Eterna", "Acros"
    };

    private FujiRecipe activeRecipe = new FujiRecipe();
    private Mode currentMode = Mode.JPEG_PROXY;
    private boolean isProcessing = false;
    private long lastProcessingTimeMs = 0;
    private String lastErrorMessage = "";
    private boolean isQMenuVisible = false;

    public FujiRecipe getActiveRecipe() {
        return activeRecipe;
    }

    public void setActiveRecipe(FujiRecipe recipe) {
        if (recipe != null) {
            this.activeRecipe = recipe;
        }
    }

    public int getCurrentPresetId() {
        return activeRecipe.filmSim.ordinal();
    }

    public void setCurrentPresetId(int id) {
        FujiRecipe.FilmSim[] sims = FujiRecipe.FilmSim.values();
        if (id >= 0 && id < sims.length) {
            activeRecipe.filmSim = sims[id];
        }
    }

    public String getCurrentPresetName() {
        return activeRecipe.filmSim.label;
    }

    /** Advances to next film simulation. */
    public int cycleNextPreset() {
        FujiRecipe.FilmSim[] sims = FujiRecipe.FilmSim.values();
        int next = (activeRecipe.filmSim.ordinal() + 1) % sims.length;
        activeRecipe.filmSim = sims[next];
        return next;
    }

    public boolean isQMenuVisible() {
        return isQMenuVisible;
    }

    public void setQMenuVisible(boolean visible) {
        this.isQMenuVisible = visible;
    }

    public Mode getCurrentMode() { return currentMode; }
    public void setCurrentMode(Mode mode) { this.currentMode = mode; }

    public boolean isProcessing() { return isProcessing; }
    public void setProcessing(boolean processing) { this.isProcessing = processing; }

    public long getLastProcessingTimeMs() { return lastProcessingTimeMs; }
    public void setLastProcessingTimeMs(long ms) { this.lastProcessingTimeMs = ms; }

    public String getLastErrorMessage() { return lastErrorMessage; }
    public void setLastErrorMessage(String err) { this.lastErrorMessage = err; }
}
