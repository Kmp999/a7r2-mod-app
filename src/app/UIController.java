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
 *  - Direct updating of FujiQMenuView unified HUD and Bottom telemetry.
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
    private boolean wheelControlsIso = true;

    // Zero-GC Pre-allocated char buffer for bottom telemetry HUD
    private final StringBuilder hudStringBuilder = new StringBuilder(128);

    // Callbacks to camera and capture systems
    public interface ControllerCallback {
        void onTriggerCapture();
        void onToggleQMenu();
        boolean isQMenuOpen();
        void onCycleFilmSim(int direction);
        void onCycleDynamicRange();
        void onCycleRecipeSlot();
        void onCycleGrain();
        void onToggleAel();
        void onToggleAfMode();
        void onToggleLiveViewLut();
        void onToggleQuickReview();
        void onResetParameter();
        void onExitApp();
    }

    private ControllerCallback callback;
    private SonyPmcaAdapter cameraAdapter;
    private FujiQMenuView qMenuView;

    public UIController(TextView statusView) {
        this(statusView, null);
    }

    public UIController(TextView statusView, TextView bottomHudView) {
        this.statusView = statusView;
        this.bottomHudView = bottomHudView;
        this.mainHandler = new Handler(Looper.getMainLooper());
    }

    private boolean liveViewLutEnabled = true;

    public boolean isLiveViewLutEnabled() {
        return liveViewLutEnabled;
    }

    public void setBottomHudView(TextView view) {
        this.bottomHudView = view;
    }

    public void setQMenuView(FujiQMenuView view) {
        this.qMenuView = view;
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
        final String ss = (cameraAdapter != null) ? cameraAdapter.getShutterSpeed() : "1/125";
        final String ap = (cameraAdapter != null) ? cameraAdapter.getAperture() : "F2.8";
        final String iso = (cameraAdapter != null) ? cameraAdapter.getIso() : "ISO 400";
        final String ev = (cameraAdapter != null) ? cameraAdapter.getEv() : "+0.0";
        final boolean ael = (cameraAdapter != null) && cameraAdapter.isAutoExposureLock();
        final String af = (cameraAdapter != null) ? cameraAdapter.getAfMode() : "AF-S";

        // Update unified FujiQMenuView HUD
        if (qMenuView != null) {
            qMenuView.updateTelemetry(ss, ap, iso, ev, ael, af);
        }

        if (bottomHudView != null) {
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    if (hudMode == HudMode.CLEAN) {
                        bottomHudView.setText("");
                        return;
                    }
                    hudStringBuilder.setLength(0);
                    hudStringBuilder.append(ss).append("  |  ")
                                    .append(ap).append("  |  ")
                                    .append(iso).append("  |  ")
                                    .append(ev).append(" EV  |  ")
                                    .append(af);
                    if (ael) {
                        hudStringBuilder.append("  [AE-L]");
                    }
                    bottomHudView.setText(hudStringBuilder.toString());
                }
            });
        }
    }

    public boolean handleKeyDown(int keyCode, KeyEvent event) {
        return handleRawKeyDown(keyCode, event);
    }

    public boolean handleKeyUp(int keyCode, KeyEvent event) {
        return handleRawKeyUp(keyCode, event);
    }

    /**
     * Dual-layer hardware key press interceptor.
     * Extracts rawCode from scanCode >= 512 or keyCode == 0.
     */
    public boolean handleRawKeyDown(int rawCode, KeyEvent event) {
        SonyKeyCodes.FujiAction action = SonyKeyCodes.resolveAction(rawCode, event);
        Log.d(TAG, "handleRawKeyDown: raw=" + rawCode + " -> " + action);

        // Always redraw QMenuView if debug logging is enabled to display the key immediately
        if (qMenuView != null && SonyKeyCodes.sDebugKeyLoggingEnabled) {
            qMenuView.postInvalidate();
        }

        // Mode 1: Quick Photo Review Overlay is Active
        if (qMenuView != null && qMenuView.isReviewing()) {
            qMenuView.clearQuickReview();
            if (action == SonyKeyCodes.FujiAction.FULL_SHUTTER) {
                if (callback != null) callback.onTriggerCapture();
            } else if (action == SonyKeyCodes.FujiAction.HALF_SHUTTER) {
                if (cameraAdapter != null) cameraAdapter.autoFocus();
            }
            return true;
        }

        // Mode 2: Q-Menu is Open
        boolean qOpen = (qMenuView != null && qMenuView.isQMenuOpen()) || (callback != null && callback.isQMenuOpen());
        if (qOpen) {
            switch (action) {
                case FULL_SHUTTER:
                    if (qMenuView != null) qMenuView.setQMenuOpen(false);
                    if (callback != null && callback.isQMenuOpen()) callback.onToggleQMenu();
                    if (callback != null) callback.onTriggerCapture();
                    return true;

                case HALF_SHUTTER:
                    if (qMenuView != null) qMenuView.setQMenuOpen(false);
                    if (callback != null && callback.isQMenuOpen()) callback.onToggleQMenu();
                    if (cameraAdapter != null) cameraAdapter.autoFocus();
                    return true;

                case C4_TRASH:
                case MENU:
                    if (qMenuView != null) qMenuView.setQMenuOpen(false);
                    if (callback != null && callback.isQMenuOpen()) callback.onToggleQMenu();
                    return true;

                case FRONT_DIAL_CW:
                    if (qMenuView != null) qMenuView.adjustCurrentSetting(2);
                    return true;
                case FRONT_DIAL_CCW:
                    if (qMenuView != null) qMenuView.adjustCurrentSetting(-2);
                    return true;

                case REAR_DIAL_CW:
                case WHEEL_CW:
                    if (qMenuView != null) qMenuView.adjustCurrentSetting(1);
                    return true;
                case REAR_DIAL_CCW:
                case WHEEL_CCW:
                    if (qMenuView != null) qMenuView.adjustCurrentSetting(-1);
                    return true;

                case DPAD_UP:
                    if (qMenuView != null) qMenuView.moveSelection(0, -1);
                    return true;
                case DPAD_DOWN:
                    if (qMenuView != null) qMenuView.moveSelection(0, 1);
                    return true;
                case DPAD_LEFT:
                    if (qMenuView != null) qMenuView.moveSelection(-1, 0);
                    return true;
                case DPAD_RIGHT:
                    if (qMenuView != null) qMenuView.moveSelection(1, 0);
                    return true;

                case CENTER:
                    if (qMenuView != null) qMenuView.resetCurrentSetting();
                    return true;

                case FN:
                    if (qMenuView != null) qMenuView.adjustCurrentSetting(0);
                    return true;

                default:
                    return true; // Consume all keys while in Q-Menu
            }
        }

        // Mode 3: Live View Viewfinder (Standard Shooting)
        switch (action) {
            case HALF_SHUTTER:
                if (cameraAdapter != null) cameraAdapter.autoFocus();
                return true;

            case FULL_SHUTTER: {
                long now = System.currentTimeMillis();
                if (now - lastS2UpTime < SHUTTER_COOLDOWN_MS) {
                    Log.d(TAG, "Shutter press dropped: Cooldown active");
                    return true;
                }
                if (!isShutterButtonPhysicalDown.compareAndSet(false, true)) {
                    Log.d(TAG, "Shutter press dropped: Button already latched down");
                    return true;
                }
                if (callback != null) callback.onTriggerCapture();
                return true;
            }

            case FRONT_DIAL_CW:
                if (cameraAdapter != null) cameraAdapter.adjustAperture(1);
                if (qMenuView != null) qMenuView.highlightSegment(2); // Highlight Aperture
                updateBottomHud();
                return true;
            case FRONT_DIAL_CCW:
                if (cameraAdapter != null) cameraAdapter.adjustAperture(-1);
                if (qMenuView != null) qMenuView.highlightSegment(2);
                updateBottomHud();
                return true;

            case REAR_DIAL_CW:
                if (cameraAdapter != null) cameraAdapter.adjustShutterSpeed(1);
                if (qMenuView != null) qMenuView.highlightSegment(1); // Highlight Shutter Speed
                updateBottomHud();
                return true;
            case REAR_DIAL_CCW:
                if (cameraAdapter != null) cameraAdapter.adjustShutterSpeed(-1);
                if (qMenuView != null) qMenuView.highlightSegment(1);
                updateBottomHud();
                return true;

            case WHEEL_CW:
                if (wheelControlsIso) {
                    if (cameraAdapter != null) cameraAdapter.stepIso(1);
                    if (qMenuView != null) qMenuView.highlightSegment(3); // Highlight ISO
                } else {
                    if (cameraAdapter != null) cameraAdapter.incrementEV(1);
                    if (qMenuView != null) qMenuView.highlightSegment(4); // Highlight EV
                }
                updateBottomHud();
                return true;
            case WHEEL_CCW:
                if (wheelControlsIso) {
                    if (cameraAdapter != null) cameraAdapter.stepIso(-1);
                    if (qMenuView != null) qMenuView.highlightSegment(3); // Highlight ISO
                } else {
                    if (cameraAdapter != null) cameraAdapter.incrementEV(-1);
                    if (qMenuView != null) qMenuView.highlightSegment(4); // Highlight EV
                }
                updateBottomHud();
                return true;

            case DPAD_RIGHT:
                if (cameraAdapter != null) cameraAdapter.stepIso(1);
                if (qMenuView != null) qMenuView.highlightSegment(3); // Highlight ISO
                updateBottomHud();
                return true;
            case DPAD_LEFT:
                if (cameraAdapter != null) cameraAdapter.stepIso(-1);
                if (qMenuView != null) qMenuView.highlightSegment(3);
                updateBottomHud();
                return true;

            case DPAD_UP:
                if (callback != null) callback.onCycleFilmSim(-1);
                if (qMenuView != null) qMenuView.highlightSegment(5); // Highlight Film Sim
                return true;
            case DPAD_DOWN:
                if (callback != null) callback.onCycleFilmSim(1);
                if (qMenuView != null) qMenuView.highlightSegment(5);
                return true;

            case CENTER:
                if (callback != null) callback.onResetParameter();
                return true;

            case C4_TRASH:
            case FN:
                if (qMenuView != null) qMenuView.setQMenuOpen(!qMenuView.isQMenuOpen());
                else if (callback != null) callback.onToggleQMenu();
                return true;

            case C1:
                if (qMenuView != null) qMenuView.cycleSplitScreenMode();
                return true;
            case C2:
                if (callback != null) callback.onCycleRecipeSlot();
                return true;
            case C3:
                if (callback != null) callback.onCycleGrain();
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
                // Return true to prevent Android OS from closing the app
                return true;

            default:
                break;
        }

        return false;
    }

    public boolean handleRawKeyUp(int rawCode, KeyEvent event) {
        SonyKeyCodes.FujiAction action = SonyKeyCodes.resolveAction(rawCode, event);

        if (action == SonyKeyCodes.FujiAction.HALF_SHUTTER) {
            if (cameraAdapter != null) cameraAdapter.cancelAutoFocus();
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
