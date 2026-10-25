package com.fujilite.a7r2;

import android.view.KeyEvent;

/**
 * Key codes for Sony Alpha 7R II (ILCE-7RM2) PlayMemories Camera Apps (PMCA) platform.
 * Defines standard and extended hardware button / dial inputs.
 */
public final class SonyKeyCodes {
    // ScalarInput / OpenMemories Keycodes on Sony A7R II
    public static final int SONY_KEY_SHUTTER_HALF  = 0x201; // 513: Half-press shutter
    public static final int SONY_KEY_SHUTTER       = 0x202; // 514: Full shutter press
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

    // Android standard fallbacks mapped by Sony firmware
    public static final int ANDROID_DEL            = KeyEvent.KEYCODE_DEL;           // 67 (Trash fallback)
    public static final int ANDROID_CLEAR          = 28;                             // KEYCODE_CLEAR
    public static final int ANDROID_MENU           = KeyEvent.KEYCODE_MENU;          // 82
    public static final int ANDROID_ENTER          = KeyEvent.KEYCODE_DPAD_CENTER;   // 23
    public static final int ANDROID_CAMERA         = KeyEvent.KEYCODE_CAMERA;        // 27 (Shutter fallback)

    private SonyKeyCodes() {}
}
