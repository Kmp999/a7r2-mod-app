package com.fujilite.a7r2;

import android.util.Log;
import android.util.SparseArray;
import android.view.KeyEvent;
import java.lang.reflect.Field;

/**
 * Comprehensive key & dial decoding for Sony Alpha 7R II (ILCE-7RM2).
 *
 * Implements a Dual-Layer Key Resolver:
 *  - Layer 1: Dynamic reflection against com.sony.scalar.sysutil.ScalarInput for ISV_KEY_* and ISV_DIAL_*.
 *  - Layer 2: Fast lookup with static fallback constants (0x201..0x20E, scanCodes, Android keyCodes).
 *
 * All public static final int constants are preserved for switch() compatibility.
 */
public final class SonyKeyCodes {
    private static final String TAG = "SonyKeyCodes";

    // ------------------------------------------------------------------------
    // Compile-Time Constants (Preserved for switch statement compatibility)
    // ------------------------------------------------------------------------
    public static final int SONY_KEY_SHUTTER_HALF  = 0x201; // 513: Half-press shutter (S1_1)
    public static final int SONY_KEY_SHUTTER       = 0x202; // 514: Full shutter press (S2)
    public static final int SONY_KEY_FN            = 0x203; // 515: Fn button
    public static final int SONY_KEY_PLAY          = 0x204; // 516: Playback button
    public static final int SONY_KEY_DIAL1_CW      = 0x205; // 517: Front dial clockwise
    public static final int SONY_KEY_DIAL1_CCW     = 0x206; // 518: Front dial counter-clockwise
    public static final int SONY_KEY_DIAL2_CW      = 0x207; // 519: Rear dial clockwise
    public static final int SONY_KEY_DIAL2_CCW     = 0x208; // 520: Rear dial counter-clockwise
    public static final int SONY_KEY_DIAL3_CW      = 0x209; // 521: Control wheel clockwise
    public static final int SONY_KEY_DIAL3_CCW     = 0x20a; // 522: Control wheel counter-clockwise
    public static final int SONY_KEY_C1            = 0x20b; // 523: Custom 1
    public static final int SONY_KEY_C2            = 0x20c; // 524: Custom 2
    public static final int SONY_KEY_C3_TRASH      = 0x20d; // 525: C3 / Trash / Delete button
    public static final int SONY_KEY_C4            = 0x20e; // 526: Custom 4 / Trash on some bodies
    public static final int SONY_KEY_AEL           = 0x20f; // 527: AE-Lock button
    public static final int SONY_KEY_AF_MF         = 0x210; // 528: AF/MF selector button
    public static final int SONY_KEY_MOVIE         = 0x211; // 529: Movie record button

    // Standard Android keycode fallbacks
    public static final int ANDROID_DEL            = KeyEvent.KEYCODE_DEL;           // 67 (Trash fallback)
    public static final int ANDROID_CLEAR          = 28;                             // KEYCODE_CLEAR
    public static final int ANDROID_MENU           = KeyEvent.KEYCODE_MENU;          // 82
    public static final int ANDROID_ENTER          = KeyEvent.KEYCODE_DPAD_CENTER;   // 23
    public static final int ANDROID_CAMERA         = KeyEvent.KEYCODE_CAMERA;        // 27 (Shutter fallback)
    public static final int ANDROID_DPAD_UP        = KeyEvent.KEYCODE_DPAD_UP;       // 19
    public static final int ANDROID_DPAD_DOWN      = KeyEvent.KEYCODE_DPAD_DOWN;     // 20
    public static final int ANDROID_DPAD_LEFT      = KeyEvent.KEYCODE_DPAD_LEFT;     // 21
    public static final int ANDROID_DPAD_RIGHT     = KeyEvent.KEYCODE_DPAD_RIGHT;    // 22

    // ------------------------------------------------------------------------
    // Normalized Fuji Key Action Mapping (18 Physical Controls)
    // ------------------------------------------------------------------------
    public enum FujiAction {
        HALF_SHUTTER,
        FULL_SHUTTER,
        FRONT_DIAL_CW,
        FRONT_DIAL_CCW,
        REAR_DIAL_CW,
        REAR_DIAL_CCW,
        WHEEL_CW,
        WHEEL_CCW,
        DPAD_UP,
        DPAD_DOWN,
        DPAD_LEFT,
        DPAD_RIGHT,
        CENTER,
        C4_TRASH,
        FN,
        C1,
        C2,
        C3,
        AEL,
        AF_MF,
        PLAY,
        MENU,
        UNKNOWN
    }

    // ------------------------------------------------------------------------
    // Dual-Layer Resolver Tables
    // ------------------------------------------------------------------------
    private static final SparseArray<FujiAction> sKeyCodeMap = new SparseArray<FujiAction>();
    private static final SparseArray<FujiAction> sScanCodeMap = new SparseArray<FujiAction>();

    // On-screen debugger telemetry
    public static volatile boolean sDebugKeyLoggingEnabled = false;
    public static volatile int sLastKeyCode = 0;
    public static volatile int sLastScanCode = 0;
    public static volatile String sLastKeyName = "NONE";

    static {
        // Step 1: Register standard fallbacks in Layer 2
        registerDefaultKeyCodes();

        // Step 2: Layer 1 - Reflectively inspect ScalarInput if present on A7R II
        inspectScalarInput();
    }

    private static void registerDefaultKeyCodes() {
        sKeyCodeMap.put(SONY_KEY_SHUTTER_HALF, FujiAction.HALF_SHUTTER);
        sKeyCodeMap.put(SONY_KEY_SHUTTER,      FujiAction.FULL_SHUTTER);
        sKeyCodeMap.put(ANDROID_CAMERA,        FujiAction.FULL_SHUTTER);

        sKeyCodeMap.put(SONY_KEY_DIAL1_CW,     FujiAction.FRONT_DIAL_CW);
        sKeyCodeMap.put(SONY_KEY_DIAL1_CCW,    FujiAction.FRONT_DIAL_CCW);
        sKeyCodeMap.put(SONY_KEY_DIAL2_CW,     FujiAction.REAR_DIAL_CW);
        sKeyCodeMap.put(SONY_KEY_DIAL2_CCW,    FujiAction.REAR_DIAL_CCW);
        sKeyCodeMap.put(SONY_KEY_DIAL3_CW,     FujiAction.WHEEL_CW);
        sKeyCodeMap.put(SONY_KEY_DIAL3_CCW,    FujiAction.WHEEL_CCW);

        sKeyCodeMap.put(ANDROID_DPAD_UP,       FujiAction.DPAD_UP);
        sKeyCodeMap.put(ANDROID_DPAD_DOWN,     FujiAction.DPAD_DOWN);
        sKeyCodeMap.put(ANDROID_DPAD_LEFT,     FujiAction.DPAD_LEFT);
        sKeyCodeMap.put(ANDROID_DPAD_RIGHT,    FujiAction.DPAD_RIGHT);
        sKeyCodeMap.put(ANDROID_ENTER,         FujiAction.CENTER);

        sKeyCodeMap.put(SONY_KEY_C3_TRASH,     FujiAction.C4_TRASH);
        sKeyCodeMap.put(SONY_KEY_C4,           FujiAction.C4_TRASH);
        sKeyCodeMap.put(ANDROID_DEL,           FujiAction.C4_TRASH);
        sKeyCodeMap.put(ANDROID_CLEAR,         FujiAction.C4_TRASH);

        sKeyCodeMap.put(SONY_KEY_FN,           FujiAction.FN);
        sKeyCodeMap.put(SONY_KEY_C1,           FujiAction.C1);
        sKeyCodeMap.put(SONY_KEY_C2,           FujiAction.C2);
        sKeyCodeMap.put(SONY_KEY_AEL,          FujiAction.AEL);
        sKeyCodeMap.put(SONY_KEY_AF_MF,        FujiAction.AF_MF);
        sKeyCodeMap.put(SONY_KEY_PLAY,         FujiAction.PLAY);
        sKeyCodeMap.put(ANDROID_MENU,          FujiAction.MENU);
    }

    private static void inspectScalarInput() {
        try {
            Class<?> scalarClass = Class.forName("com.sony.scalar.sysutil.ScalarInput");
            Log.i(TAG, "ScalarInput found; resolving hardware keycodes via reflection...");

            mapScalarField(scalarClass, "ISV_KEY_S1_1",             FujiAction.HALF_SHUTTER);
            mapScalarField(scalarClass, "ISV_KEY_S2",               FujiAction.FULL_SHUTTER);
            mapScalarField(scalarClass, "ISV_KEY_FN",               FujiAction.FN);
            mapScalarField(scalarClass, "ISV_KEY_PLAY",             FujiAction.PLAY);
            mapScalarField(scalarClass, "ISV_KEY_DELETE",           FujiAction.C4_TRASH);
            mapScalarField(scalarClass, "ISV_KEY_CUSTOM1",          FujiAction.C1);
            mapScalarField(scalarClass, "ISV_KEY_CUSTOM2",          FujiAction.C2);
            mapScalarField(scalarClass, "ISV_KEY_CUSTOM3",          FujiAction.C3);
            mapScalarField(scalarClass, "ISV_KEY_AEL",              FujiAction.AEL);
            mapScalarField(scalarClass, "ISV_KEY_STAMINA",          FujiAction.AF_MF);
            mapScalarField(scalarClass, "ISV_KEY_MENU",             FujiAction.MENU);
            mapScalarField(scalarClass, "ISV_KEY_ENTER",            FujiAction.CENTER);
            mapScalarField(scalarClass, "ISV_KEY_UP",               FujiAction.DPAD_UP);
            mapScalarField(scalarClass, "ISV_KEY_DOWN",             FujiAction.DPAD_DOWN);
            mapScalarField(scalarClass, "ISV_KEY_LEFT",             FujiAction.DPAD_LEFT);
            mapScalarField(scalarClass, "ISV_KEY_RIGHT",            FujiAction.DPAD_RIGHT);

            // Dial rotations
            mapScalarField(scalarClass, "ISV_DIAL_1_CLOCKWISE",        FujiAction.FRONT_DIAL_CW);
            mapScalarField(scalarClass, "ISV_DIAL_1_COUNTERCLOCKWISE", FujiAction.FRONT_DIAL_CCW);
            mapScalarField(scalarClass, "ISV_DIAL_2_CLOCKWISE",        FujiAction.REAR_DIAL_CW);
            mapScalarField(scalarClass, "ISV_DIAL_2_COUNTERCLOCKWISE", FujiAction.REAR_DIAL_CCW);
            mapScalarField(scalarClass, "ISV_DIAL_3_CLOCKWISE",        FujiAction.WHEEL_CW);
            mapScalarField(scalarClass, "ISV_DIAL_3_COUNTERCLOCKWISE", FujiAction.WHEEL_CCW);

        } catch (ClassNotFoundException e) {
            Log.d(TAG, "ScalarInput not present (running on host or emulator); using fallback table.");
        } catch (Throwable t) {
            Log.w(TAG, "Error inspecting ScalarInput fields", t);
        }
    }

    private static void mapScalarField(Class<?> cls, String fieldName, FujiAction action) {
        try {
            Field field = cls.getField(fieldName);
            int code = field.getInt(null);
            if (code > 0) {
                sKeyCodeMap.put(code, action);
                Log.d(TAG, "Mapped ScalarInput." + fieldName + " (" + code + ") -> " + action);
            }
        } catch (NoSuchFieldException ignored) {
        } catch (Throwable t) {
            Log.w(TAG, "Could not map field " + fieldName, t);
        }
    }

    /**
     * Resolves an incoming KeyEvent using the Dual-Layer Key Resolver.
     * Checks keyCode first, then scanCode, with full debug telemetry tracking.
     */
    public static FujiAction resolveAction(int keyCode, KeyEvent event) {
        sLastKeyCode = keyCode;
        int scanCode = (event != null) ? event.getScanCode() : 0;
        sLastScanCode = scanCode;

        // Layer 1: Direct keyCode check
        FujiAction action = sKeyCodeMap.get(keyCode);
        if (action != null) {
            sLastKeyName = action.name();
            return action;
        }

        // Layer 2: ScanCode fallback
        if (scanCode > 0) {
            action = sScanCodeMap.get(scanCode);
            if (action != null) {
                sLastKeyName = action.name();
                return action;
            }
        }

        sLastKeyName = "UNKNOWN(" + keyCode + "/" + scanCode + ")";
        return FujiAction.UNKNOWN;
    }

    private SonyKeyCodes() {}
}
