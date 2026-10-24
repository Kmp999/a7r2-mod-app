# Sony PMCA SDK Contract

This document outlines the API surface we expect from the Sony Android PMCA environment (based on Android 4.x / 2.3.x APIs and `com.sony.scalar` extensions).

## Core Android APIs
| API / Class | Status | Requirement | Note |
| :--- | :--- | :--- | :--- |
| `android.hardware.Camera` | **VERIFIED** | REQUIRED | Standard API for capturing preview/pictures. |
| `Camera.PreviewCallback` | **VERIFIED** | REQUIRED | Provides NV21 byte arrays. |
| `Camera.PictureCallback` | **VERIFIED** | REQUIRED | Provides JPEG byte arrays. |
| `Camera.Parameters` | **VERIFIED** | REQUIRED | To set resolution and preview formats. |
| `android.view.SurfaceView` | **VERIFIED** | REQUIRED | For camera preview rendering. |
| `android.graphics.Bitmap` | **VERIFIED** | OPTIONAL | Software decode fallback. |

## Sony Scalar Extensions (`com.sony.scalar.*`)
| API / Class | Status | Requirement | Note |
| :--- | :--- | :--- | :--- |
| `com.sony.scalar.hardware.CameraEx` | UNVERIFIED | OPTIONAL | Needed for deep hardware control (e.g. continuous shooting, internal profiles). |
| `CameraEx.ParametersModifier` | UNVERIFIED | OPTIONAL | Allows modifying Sony-specific picture profiles. |
| `CameraEx.AutoPictureReviewControl` | UNVERIFIED | OPTIONAL | Prevents default Sony UI from interrupting our app. |

## Contract Action Plan
If `com.sony.scalar.hardware.CameraEx` is verified and supports applying color matrices natively:
*   We use **MODE 1: INTERNAL_PIPELINE_MODE**.
*   We skip our C++ JNI layer completely for image processing, achieving near 0ms overhead.

If `com.sony.scalar` is unavailable or locked:
*   We use standard `android.hardware.Camera`.
*   We use **MODE 2: JPEG_PROXY_MODE** or **MODE 3: PREVIEW_ONLY_MODE**.
*   We route `PreviewCallback` NV21 buffers directly to C++ JNI for processing.
