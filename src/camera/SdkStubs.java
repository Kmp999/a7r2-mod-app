package com.fujilite.a7r2;

/**
 * Stub interface definitions for com.sony.scalar.* APIs.
 *
 * NOTE: The Sony PMCA Scalar SDK is proprietary and unreleased officially.
 * Methods here must be probed via reflection on actual camera hardware.
 * Mark all unverified methods with TODO(SONY_SDK).
 */
public final class SdkStubs {

    // TODO(SONY_SDK): Verify exact class name com.sony.scalar.hardware.CameraEx
    public static final String SONY_CAMERA_EX_CLASS = "com.sony.scalar.hardware.CameraEx";

    // TODO(SONY_SDK): Verify availability of picture profile parameter modification
    public static final String METHOD_CREATE_PARAMETERS_MODIFIER = "createParametersModifier";

    // TODO(SONY_SDK): Verify availability of auto-review control
    public static final String SONY_AUTO_REVIEW_CONTROL_CLASS = "com.sony.scalar.hardware.CameraEx$AutoPictureReviewControl";

    private SdkStubs() {}

    /**
     * Checks via runtime reflection whether Sony PMCA Scalar extensions exist in this Dalvik environment.
     */
    public static boolean isSonyScalarAvailable() {
        try {
            Class.forName(SONY_CAMERA_EX_CLASS);
            return true;
        } catch (ClassNotFoundException e) {
            return false;
        }
    }
}
