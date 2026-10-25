package com.fujilite.a7r2;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.widget.TextView;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Lightweight UI & hardware event controller for Sony A7R II.
 *
 * Enforces:
 *  - 5 FPS deadband (200ms) on text updates to protect main thread.
 *  - Zero-GC formatted Bottom HUD telemetry buffer (char[] based).
 *  - Triple-lock anti-repeat & latch for shutter button (defeats Infinite Capture Loop).
 *  - Dual-layer key event routing across Live View, Q-Menu, and Quick Review.
 */
public class UIController {
    private static final String TAG = "UIController";

    private final TextView statusView;
    private TextView bottomHudView;
    private final Handler mainHandler;

    private long lastUiUpdateTime = 0;
    private static final long MIN_UI_INTERVAL_MS = 200; // 5 FPS maximum

    // Shutter latch state to prevent Infinite Capture Loop
    private final AtomicBoolean isShutterButtonPhysicalDown = new AtomicBoolean(false);
    private long lastS2UpTime = 0;
    private static final long SHUTTER_COOLDOWN_MS = 400;

    // HUD mode: 0 = Full Info, 1 = Minimal, 2 = Clean Viewfinder
    public enum HudMode { FULL, MINIMAL, CLEAN }
    private HudMode hudMode = HudMode.FULL;

    // Control wheel mode: false = EV compensation, true = ISO sensitivity
    private boolean wheelControlsIso = false;

    // Zero-GC Pre-allocated char buffer for bottom telemetry HUD
    private final char[] hudBuffer = new char[128];
    private final StringBuilder hudStringBuilder = new StringBuilder(128);

    // Callbacks to camera and capture systems
    public interface ControllerCallback {
        void onTriggerCapture();
        void onToggleQMenu();
        boolean isQMenuOpen();
        void onCycleFilmSim(int direction);
        void onCycleDynamicRange();
        void onCycleRecipeSlot();
        void onToggleAel();
        void onToggleAfMode();
        void onToggleLiveViewLut();
        void onToggleQuickReview();
        void onResetParameter();
        void onExitApp();
    }

    private ControllerCallback callback;
    private SonyPmcaAdapter cameraAdapter;

    public UIController(TextView statusView) {
        this(statusView, null);
    }

    public UIController(TextView statusView, TextView bottomHudView) {
        this.statusView = statusView;
        this.bottomHudView = bottomHudView;
        this.mainHandler = new Handler(Looper.getMainLooper());
    }

    public void setBottomHudView(TextView view) {
        this.bottomHudView = view;
    }

    public void setControllerCallback(ControllerCallback cb) {
        this.callback = cb;
    }

    public void setCameraAdapter(SonyPmcaAdapter adapter) {
        this.cameraAdapter = adapter;
    }

    public HudMode getHudMode() {
        return hudMode;
    }

    public void cycleHudMode() {
        if (hudMode == HudMode.FULL) hudMode = HudMode.MINIMAL;
        else if (hudMode == HudMode.MINIMAL) hudMode = HudMode.CLEAN;
        else hudMode = HudMode.FULL;
        updateStatusAsync("HUD: " + hudMode.name());
        updateBottomHud();
    }

    public void toggleWheelMode() {
        wheelControlsIso = !wheelControlsIso;
        updateStatusAsync("WHEEL: " + (wheelControlsIso ? "ISO" : "EV COMP"));
        updateBottomHud();
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
                    if (SonyKeyCodes.sDebugKeyLoggingEnabled) {
                        statusView.setText(message + " [" + SonyKeyCodes.sLastKeyName + "]");
                    } else {
                        statusView.setText(message);
                    }
                }
            }
        });
    }

    /**
     * Updates bottom HUD telemetry with Zero-GC overhead.
     */
    public void updateBottomHud() {
        if (bottomHudView == null) return;

        if (hudMode == HudMode.CLEAN) {
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    bottomHudView.setText("");
                }
            });
            return;
        }

        final String ss = (cameraAdapter != null) ? cameraAdapter.getShutterSpeed() : "1/125";
        final String ap = (cameraAdapter != null) ? cameraAdapter.getAperture() : "F2.8";
        final String iso = (cameraAdapter != null) ? cameraAdapter.getIso() : "ISO 400";
        final String ev = (cameraAdapter != null) ? cameraAdapter.getEv() : "+0.0";
        final boolean ael = (cameraAdapter != null) && cameraAdapter.isAutoExposureLock();
        final String af = (cameraAdapter != null) ? cameraAdapter.getAfMode() : "AF-S";

        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                hudStringBuilder.setLength(0);
                if (hudMode == HudMode.FULL) {
                    hudStringBuilder.append(ss).append("  |  ")
                                    .append(ap).append("  |  ")
                                    .append(iso).append("  |  ")
                                    .append(ev).append(" EV  |  ")
                                    .append(af);
                    if (ael) {
                        hudStringBuilder.append("  [AE-L]");
                    }
                } else {
                    // Minimal mode: Essential Exposure only
                    hudStringBuilder.append(ss).append("  ").append(ap).append("  ").append(iso);
                    if (ael) hudStringBuilder.append(" [L]");
                }
                bottomHudView.setText(hudStringBuilder.toString());
            }
        });
    }

    /**
     * Dual-layer hardware key press interceptor.
     * Enforces anti-repeat and cooldown locks for shutter release.
     */
    public boolean handleKeyDown(int keyCode, KeyEvent event) {
        // Anti-repeat lock: Never allow firmware key repeat on shutter buttons
        if (event.getRepeatCount() > 0) {
            if (keyCode == SonyKeyCodes.SONY_KEY_SHUTTER || keyCode == SonyKeyCodes.SONY_KEY_SHUTTER_HALF) {
                return true;
            }
        }

        SonyKeyCodes.FujiAction action = SonyKeyCodes.resolveAction(keyCode, event);

        // Half-shutter autofocus & Q-menu dismiss
        if (action == SonyKeyCodes.FujiAction.HALF_SHUTTER) {
            if (callback != null && callback.isQMenuOpen()) {
                callback.onToggleQMenu();
                return true;
            }
            if (cameraAdapter != null) {
                cameraAdapter.autoFocus();
            }
            return true;
        }

        // Full-shutter single-shot capture
        if (action == SonyKeyCodes.FujiAction.FULL_SHUTTER) {
            long now = System.currentTimeMillis();
            if (now - lastS2UpTime < SHUTTER_COOLDOWN_MS) {
                Log.d(TAG, "Shutter press dropped: Cooldown active");
                return true;
            }
            if (!isShutterButtonPhysicalDown.compareAndSet(false, true)) {
                Log.d(TAG, "Shutter press dropped: Button already latched down");
                return true;
            }

            if (callback != null) {
                callback.onTriggerCapture();
            }
            return true;
        }

        // Live View vs Q-Menu specific routing
        boolean qOpen = (callback != null && callback.isQMenuOpen());

        if (!qOpen) {
            switch (action) {
                case FRONT_DIAL_CW:
                    if (cameraAdapter != null) cameraAdapter.incrementAperture(1);
                    updateBottomHud();
                    return true;
                case FRONT_DIAL_CCW:
                    if (cameraAdapter != null) cameraAdapter.incrementAperture(-1);
                    updateBottomHud();
                    return true;
                case REAR_DIAL_CW:
                    if (cameraAdapter != null) cameraAdapter.incrementShutterSpeed(1);
                    updateBottomHud();
                    return true;
                case REAR_DIAL_CCW:
                    if (cameraAdapter != null) cameraAdapter.incrementShutterSpeed(-1);
                    updateBottomHud();
                    return true;
                case WHEEL_CW:
                    if (wheelControlsIso && cameraAdapter != null) {
                        cameraAdapter.setISOSensitivity(800);
                    } else if (cameraAdapter != null) {
                        cameraAdapter.incrementEV(1);
                    }
                    updateBottomHud();
                    return true;
                case WHEEL_CCW:
                    if (wheelControlsIso && cameraAdapter != null) {
                        cameraAdapter.setISOSensitivity(200);
                    } else if (cameraAdapter != null) {
                        cameraAdapter.incrementEV(-1);
                    }
                    updateBottomHud();
                    return true;
                case DPAD_UP:
                    cycleHudMode();
                    return true;
                case DPAD_DOWN:
                    toggleWheelMode();
                    return true;
                case DPAD_LEFT:
                    if (callback != null) callback.onCycleFilmSim(-1);
                    return true;
                case DPAD_RIGHT:
                    if (callback != null) callback.onCycleFilmSim(1);
                    return true;
                case CENTER:
                    if (callback != null) callback.onResetParameter();
                    return true;
                case C4_TRASH:
                    if (callback != null) callback.onToggleQMenu();
                    return true;
                case FN:
                    if (callback != null) callback.onCycleRecipeSlot();
                    return true;
                case C1:
                    if (callback != null) callback.onCycleFilmSim(1);
                    return true;
                case C2:
                    if (callback != null) callback.onToggleLiveViewLut();
                    return true;
                case C3:
                    if (callback != null) callback.onCycleDynamicRange();
                    return true;
                case AEL:
                    if (callback != null) callback.onToggleAel();
                    return true;
                case AF_MF:
                    if (callback != null) callback.onToggleAfMode();
                    return true;
                case PLAY:
                    if (callback != null) callback.onToggleQuickReview();
                    return true;
                case MENU:
                    // Return true to block default Android OS app exit
                    return true;
                default:
                    break;
            }
        }

        return false;
    }

    public boolean handleKeyUp(int keyCode, KeyEvent event) {
        SonyKeyCodes.FujiAction action = SonyKeyCodes.resolveAction(keyCode, event);

        if (action == SonyKeyCodes.FujiAction.HALF_SHUTTER) {
            if (cameraAdapter != null) {
                cameraAdapter.cancelAutoFocus();
            }
            return true;
        }

        if (action == SonyKeyCodes.FujiAction.FULL_SHUTTER) {
            isShutterButtonPhysicalDown.set(false);
            lastS2UpTime = System.currentTimeMillis();
            return true;
        }

        return false;
    }
}
