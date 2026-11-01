package com.fujilite.a7r2;

public class AppState {
    public enum Mode {
        INTERNAL_PIPELINE,
        JPEG_PROXY,
        PREVIEW_ONLY,
        ASYNC_FULLRES
    }

    public enum OutputResolutionMode {
        FAST_10MP("10.6MP (FAST)", 3976, 2652, 2),
        MASTER_42MP("42.4MP (MASTER)", 7952, 5304, 1);

        public final String label;
        public final int width;
        public final int height;
        public final int inSampleSize;

        OutputResolutionMode(String label, int width, int height, int inSampleSize) {
            this.label = label;
            this.width = width;
            this.height = height;
            this.inSampleSize = inSampleSize;
        }
    }

    // Film simulations matching FujiRecipe (Fujifilm, Kodak, Cinema stocks)
    public static final String[] PRESET_NAMES = {
        "Provia", "Velvia", "Astia", "Classic Chr", "Classic Neg",
        "Pro Neg Hi", "Pro Neg Std", "Eterna", "Acros",
        "Reala 500D", "Eterna 250D", "F125", "Fuji 3510", "3513DI D60",
        "Portra 5205", "Vision 5218", "Kodak 2383", "2383 D55", "2383 D65", "Filmstock 50",
        "AeroLux IR", "Teal & Orange", "Tension Green", "Landscape", "Vintage Pink", "Neon Cyber"
    };

    private FujiRecipe activeRecipe = new FujiRecipe();
    private Mode currentMode = Mode.JPEG_PROXY;
    private OutputResolutionMode outputResolutionMode = OutputResolutionMode.FAST_10MP;
    private boolean isProcessing = false;
    private long lastProcessingTimeMs = 0;
    private String lastErrorMessage = "";
    private boolean isQMenuVisible = false;

    public OutputResolutionMode getOutputResolutionMode() {
        return outputResolutionMode;
    }

    public void setOutputResolutionMode(OutputResolutionMode mode) {
        if (mode != null) {
            this.outputResolutionMode = mode;
        }
    }

    public OutputResolutionMode toggleOutputResolutionMode() {
        if (this.outputResolutionMode == OutputResolutionMode.FAST_10MP) {
            this.outputResolutionMode = OutputResolutionMode.MASTER_42MP;
        } else {
            this.outputResolutionMode = OutputResolutionMode.FAST_10MP;
        }
        return this.outputResolutionMode;
    }

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
