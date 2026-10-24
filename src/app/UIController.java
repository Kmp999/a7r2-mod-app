package com.fujilite.a7r2;

import android.os.Handler;
import android.os.Looper;
import android.widget.TextView;

/**
 * Lightweight UI update controller with rate limiting / deadbanding (2-5 FPS).
 * Never blocks camera thread, strictly queues via Looper.
 */
public class UIController {
    private final TextView statusView;
    private final Handler mainHandler;
    private long lastUiUpdateTime = 0;
    private static final long MIN_UI_INTERVAL_MS = 200; // 5 FPS maximum

    public UIController(TextView statusView) {
        this.statusView = statusView;
        this.mainHandler = new Handler(Looper.getMainLooper());
    }

    public void updateStatusAsync(final String message) {
        long now = System.currentTimeMillis();
        if (now - lastUiUpdateTime < MIN_UI_INTERVAL_MS) {
            return; // Drop excessive UI updates to maintain hot path performance
        }
        lastUiUpdateTime = now;

        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                if (statusView != null) {
                    statusView.setText(message);
                }
            }
        });
    }
}
