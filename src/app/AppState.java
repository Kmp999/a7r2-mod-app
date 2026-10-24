package com.fujilite.a7r2;

public class AppState {
    public enum Mode {
        INTERNAL_PIPELINE,
        JPEG_PROXY,
        PREVIEW_ONLY,
        ASYNC_FULLRES
    }

    private int currentPresetId = ColorEngine.PRESET_PROVIA_LIKE;
    private Mode currentMode = Mode.JPEG_PROXY;
    private boolean isProcessing = false;
    private long lastProcessingTimeMs = 0;
    private String lastErrorMessage = "";

    public int getCurrentPresetId() { return currentPresetId; }
    public void setCurrentPresetId(int id) { this.currentPresetId = id; }

    public Mode getCurrentMode() { return currentMode; }
    public void setCurrentMode(Mode mode) { this.currentMode = mode; }

    public boolean isProcessing() { return isProcessing; }
    public void setProcessing(boolean processing) { this.isProcessing = processing; }

    public long getLastProcessingTimeMs() { return lastProcessingTimeMs; }
    public void setLastProcessingTimeMs(long ms) { this.lastProcessingTimeMs = ms; }

    public String getLastErrorMessage() { return lastErrorMessage; }
    public void setLastErrorMessage(String err) { this.lastErrorMessage = err; }
}
