package com.fujilite.a7r2;

import android.util.Log;
import android.util.SparseArray;
import android.view.KeyEvent;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

/**
 * Comprehensive key & dial decoding for Sony Alpha 7R II (ILCE-7RM2).
 *
 * Implements a Dual-Layer Key Resolver:
 *  - Layer 1: Dynamic reflection against com.sony.scalar.sysutil.ScalarInput
 *             scanning ALL ISV_KEY_* and ISV_DIAL_* fields.
 *  - Layer 2: Fast lookup with static fallback constants (512..650, scanCodes, Android keyCodes).
 *
 * On Sony A7R II hardware, dials and buttons send events with keyCode == 0 and scanCode >= 512.
 * Both keyCode and scanCode are resolved transparently.
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

    // On-screen debugger telemetry
    public static volatile boolean sDebugKeyLoggingEnabled = true; // Enabled by default for easy tuning on A7R II
    public static volatile int sLastKeyCode = 0;
    public static volatile int sLastScanCode = 0;
    public static volatile String sLastKeyName = "NONE";

    static {
        registerDefaultKeyCodes();
        inspectScalarInput();
    }

    private static void registerDefaultKeyCodes() {
        // Core Shutter
        sKeyCodeMap.put(0x201, FujiAction.HALF_SHUTTER);
        sKeyCodeMap.put(0x202, FujiAction.FULL_SHUTTER);
        sKeyCodeMap.put(513, FujiAction.HALF_SHUTTER);
        sKeyCodeMap.put(514, FujiAction.FULL_SHUTTER);

        // Menu & Navigation
        sKeyCodeMap.put(515, FujiAction.MENU);
        sKeyCodeMap.put(516, FujiAction.CENTER);
        
        sKeyCodeMap.put(517, FujiAction.DPAD_UP);
        sKeyCodeMap.put(103, FujiAction.DPAD_UP);
        
        sKeyCodeMap.put(518, FujiAction.DPAD_DOWN);
        sKeyCodeMap.put(108, FujiAction.DPAD_DOWN);
        
        sKeyCodeMap.put(591, FujiAction.DPAD_LEFT);
        sKeyCodeMap.put(105, FujiAction.DPAD_LEFT);
        
        sKeyCodeMap.put(592, FujiAction.DPAD_RIGHT);
        sKeyCodeMap.put(106, FujiAction.DPAD_RIGHT);

        // Dials
        sKeyCodeMap.put(519, FujiAction.WHEEL_CW);
        sKeyCodeMap.put(520, FujiAction.WHEEL_CCW);
        sKeyCodeMap.put(0x209, FujiAction.WHEEL_CW);
        sKeyCodeMap.put(0x20a, FujiAction.WHEEL_CCW);

        sKeyCodeMap.put(525, FujiAction.FRONT_DIAL_CW);
        sKeyCodeMap.put(534, FujiAction.FRONT_DIAL_CW);
        sKeyCodeMap.put(0x205, FujiAction.FRONT_DIAL_CW);
        
        sKeyCodeMap.put(526, FujiAction.FRONT_DIAL_CCW);
        sKeyCodeMap.put(535, FujiAction.FRONT_DIAL_CCW);
        sKeyCodeMap.put(0x206, FujiAction.FRONT_DIAL_CCW);

        sKeyCodeMap.put(527, FujiAction.REAR_DIAL_CW);
        sKeyCodeMap.put(536, FujiAction.REAR_DIAL_CW);
        sKeyCodeMap.put(0x207, FujiAction.REAR_DIAL_CW);
        
        sKeyCodeMap.put(528, FujiAction.REAR_DIAL_CCW);
        sKeyCodeMap.put(537, FujiAction.REAR_DIAL_CCW);
        sKeyCodeMap.put(0x208, FujiAction.REAR_DIAL_CCW);

        // Custom Buttons & Toggles
        sKeyCodeMap.put(521, FujiAction.FN);
        sKeyCodeMap.put(0x203, FujiAction.FN);
        
        sKeyCodeMap.put(532, FujiAction.AEL);
        sKeyCodeMap.put(533, FujiAction.AF_MF);
        sKeyCodeMap.put(538, FujiAction.PLAY);

        sKeyCodeMap.put(588, FujiAction.C1);
        sKeyCodeMap.put(622, FujiAction.C1);

        sKeyCodeMap.put(589, FujiAction.C2);
        sKeyCodeMap.put(623, FujiAction.C2);

        sKeyCodeMap.put(590, FujiAction.C3);
        sKeyCodeMap.put(624, FujiAction.C3);

        sKeyCodeMap.put(595, FujiAction.C4_TRASH);
        sKeyCodeMap.put(625, FujiAction.C4_TRASH);
        sKeyCodeMap.put(0x20d, FujiAction.C4_TRASH);

        // Android standard key codes
        sKeyCodeMap.put(KeyEvent.KEYCODE_CAMERA,        FujiAction.FULL_SHUTTER);
        sKeyCodeMap.put(KeyEvent.KEYCODE_DPAD_UP,       FujiAction.DPAD_UP);
        sKeyCodeMap.put(KeyEvent.KEYCODE_DPAD_DOWN,     FujiAction.DPAD_DOWN);
        sKeyCodeMap.put(KeyEvent.KEYCODE_DPAD_LEFT,     FujiAction.DPAD_LEFT);
        sKeyCodeMap.put(KeyEvent.KEYCODE_DPAD_RIGHT,    FujiAction.DPAD_RIGHT);
        sKeyCodeMap.put(KeyEvent.KEYCODE_DPAD_CENTER,   FujiAction.CENTER);
        sKeyCodeMap.put(KeyEvent.KEYCODE_ENTER,         FujiAction.CENTER);
        sKeyCodeMap.put(KeyEvent.KEYCODE_DEL,           FujiAction.C4_TRASH);
        sKeyCodeMap.put(28,                             FujiAction.C4_TRASH); // KEYCODE_CLEAR
        sKeyCodeMap.put(KeyEvent.KEYCODE_MENU,          FujiAction.MENU);
        sKeyCodeMap.put(KeyEvent.KEYCODE_PAGE_UP,       FujiAction.REAR_DIAL_CW);
        sKeyCodeMap.put(KeyEvent.KEYCODE_PAGE_DOWN,     FujiAction.REAR_DIAL_CCW);
    }

    private static void inspectScalarInput() {
        try {
            Class<?> scalarClass = Class.forName("com.sony.scalar.sysutil.ScalarInput");
            Log.i(TAG, "ScalarInput found; reflecting all hardware key & dial fields...");

            Field[] fields = scalarClass.getFields();
            for (Field field : fields) {
                if (!Modifier.isStatic(field.getModifiers()) || field.getType() != int.class) {
                    continue;
                }
                String name = field.getName();
                int code = field.getInt(null);
                if (code <= 0) continue;

                FujiAction action = matchFieldToAction(name);
                if (action != null) {
                    sKeyCodeMap.put(code, action);
                    Log.d(TAG, "Reflected ScalarInput." + name + " = " + code + " -> " + action);
                }
            }
        } catch (ClassNotFoundException e) {
            Log.d(TAG, "ScalarInput not present (running on host or emulator); using fallback table.");
        } catch (Throwable t) {
            Log.w(TAG, "Error inspecting ScalarInput fields", t);
        }
    }

    private static FujiAction matchFieldToAction(String name) {
        String u = name.toUpperCase();
        if (u.contains("S1_1") || u.contains("S1_2") || u.contains("HALF")) return FujiAction.HALF_SHUTTER;
        if (u.contains("S2") || u.contains("SHUTTER")) return FujiAction.FULL_SHUTTER;
        if (u.contains("FN")) return FujiAction.FN;
        if (u.contains("PLAY")) return FujiAction.PLAY;
        if (u.contains("DELETE") || u.contains("TRASH")) return FujiAction.C4_TRASH;
        if (u.contains("CUSTOM1") || u.endsWith("_C1")) return FujiAction.C1;
        if (u.contains("CUSTOM2") || u.endsWith("_C2")) return FujiAction.C2;
        if (u.contains("CUSTOM3") || u.endsWith("_C3")) return FujiAction.C3;
        if (u.contains("CUSTOM4") || u.endsWith("_C4")) return FujiAction.C4_TRASH;
        if (u.contains("AEL")) return FujiAction.AEL;
        if (u.contains("STAMINA") || u.contains("AF_MF")) return FujiAction.AF_MF;
        if (u.contains("MENU")) return FujiAction.MENU;
        if (u.contains("ENTER") || u.contains("CENTER")) return FujiAction.CENTER;
        if (u.contains("UP")) return FujiAction.DPAD_UP;
        if (u.contains("DOWN")) return FujiAction.DPAD_DOWN;
        if (u.contains("LEFT")) return FujiAction.DPAD_LEFT;
        if (u.contains("RIGHT")) return FujiAction.DPAD_RIGHT;

        // Dials
        if (u.contains("DIAL_1") || u.contains("DIAL1")) {
            return (u.contains("COUNTER") || u.contains("CCW")) ? FujiAction.FRONT_DIAL_CCW : FujiAction.FRONT_DIAL_CW;
        }
        if (u.contains("DIAL_2") || u.contains("DIAL2")) {
            return (u.contains("COUNTER") || u.contains("CCW")) ? FujiAction.REAR_DIAL_CCW : FujiAction.REAR_DIAL_CW;
        }
        if (u.contains("DIAL_3") || u.contains("DIAL3") || u.contains("WHEEL")) {
            return (u.contains("COUNTER") || u.contains("CCW")) ? FujiAction.WHEEL_CCW : FujiAction.WHEEL_CW;
        }

        return null;
    }

    /**
     * Resolves an incoming KeyEvent using the Dual-Layer Key Resolver.
     * On Sony A7R II, if scanCode >= 512 or keyCode == 0, prioritize scanCode!
     */
    public static FujiAction resolveAction(int rawCode, KeyEvent event) {
        int scan = (event != null) ? event.getScanCode() : 0;
        int key = (event != null) ? event.getKeyCode() : 0;
        sLastKeyCode = key;
        sLastScanCode = scan;

        // Choose effective code: On Sony A7R II, physical dials and buttons have scanCode >= 512 or key == 0
        int effective = (scan >= 512 || key == 0) ? (scan > 0 ? scan : rawCode) : ((rawCode > 0) ? rawCode : key);

        FujiAction action = sKeyCodeMap.get(effective);
        if (action != null) {
            sLastKeyName = action.name();
            return action;
        }

        if (scan > 0) {
            action = sKeyCodeMap.get(scan);
            if (action != null) {
                sLastKeyName = action.name();
                return action;
            }
        }

        if (key > 0) {
            action = sKeyCodeMap.get(key);
            if (action != null) {
                sLastKeyName = action.name();
                return action;
            }
        }

        sLastKeyName = "UNKNOWN(" + rawCode + "/s:" + scan + "/k:" + key + ")";
        return FujiAction.UNKNOWN;
    }

    private SonyKeyCodes() {}
}
