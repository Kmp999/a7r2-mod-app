# Architecture: Fuji Lite for Sony A7R II

## Architecture Diagram

```
+-----------------------------------------------------------------------------------+
|                            SONY A7R II CAMERA APP                                 |
|                                                                                   |
|  +---------------------------+       +-----------------------------------------+  |
|  |       UI Thread           |       |           Capture Thread (Async)        |  |
|  |                           |       |                                         |  |
|  | [Preset Selector]         | <---- | [Sony PMCA Camera Adapter]              |  |
|  | [Capture Button]          | ----> |   - com.sony.scalar.hardware.CameraEx   |  |
|  | [Status / Timing Display] |       |   - PictureCallback / PreviewCallback   |  |
|  +---------------------------+       +-----------------------------------------+  |
|                                                          |                        |
|                                                          v (YUV / Proxy JPEG)     |
|  +---------------------------+       +-----------------------------------------+  |
|  |   Storage / Memory Pool   |       |             C++ JNI Layer               |  |
|  |                           | <---> |                                         |  |
|  | - Pre-allocated byte[]    |       | [Fixed-Point Color Engine]              |  |
|  | - Pre-computed 1D LUTs    |       |   - 3x3 Matrix Multiply                 |  |
|  | - SD Card Writer          |       |   - 1D Tone Curve Application           |  |
|  +---------------------------+       |   - YUV -> RGB -> Filter -> YUV/JPEG    |  |
|                                      +-----------------------------------------+  |
+-----------------------------------------------------------------------------------+
```

## Module Boundaries

### 1. App Layer (`src/app/`)
*   **`main_entry`**: The Android Activity (`BaseActivity`) acting as the PMCA entry point.
*   **`app_state`**: Manages current preset, UI mode, and performance flags.
*   **`ui_controller`**: Extremely lightweight standard Android Canvas/SurfaceView or minimalistic XML layouts to avoid GC pauses.

### 2. Camera Adapter Layer (`src/camera/`)
*   **`camera_adapter_interface`**: Agnostic interface `takePicture()`, `startPreview()`.
*   **`sony_pmca_adapter`**: Specific implementation using `com.sony.scalar` classes (reflection or provided stubs). Maps unknown/unverified APIs as `TODO(SONY_SDK)`.

### 3. Portable Color Engine (`src/color/`)
*   Written in **C/C++ (JNI)** for maximum speed. No floating-point math in the hot loop.
*   **`fixed_point_matrix`**: Converts the 3x3 RGB matrix to a `Q8.8` or `Q16.16` fixed-point representation.
*   **`tone_curve_lut`**: 256-element 1D arrays for R, G, B channels, and luminance mapping.
*   **`preset_manager`**: Loads configurations (matrix + LUT params) statically at boot.

### 4. Memory Model
*   **Zero per-frame allocation.**
*   A static `MemoryPool` pre-allocates maximum required buffers (e.g., 1920x1080 * 4 bytes for ARGB = 8MB, or NV21 equivalent) upon app startup.
*   LUTs (256 bytes per channel) are computed during preset selection, NOT during image capture.

### 5. UI Model
*   Simple `SurfaceView`. Updates are throttled to 2-5 FPS maximum. UI thread only dispatches commands and receives lightweight status messages (e.g., `"Processed in 312ms"`).

### 6. Storage Model
*   `OutputWriter` runs on a dedicated background thread.
*   Writes processed JPEGs directly to the SD Card (DCIM folder format).
