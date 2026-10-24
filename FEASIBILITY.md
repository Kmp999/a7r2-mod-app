# Feasibility Analysis for 500 ms Processing on Sony A7R II

## Timing Feasibility

### Is 500 ms possible from shutter press?
**Verdict:** **Unlikely.**
The A7R II (BIONZ X processor) has significant mechanical and readout latency. Just capturing the full 42 MP image, encoding the JPEG/RAW, and moving it to the Android app space takes >500 ms even for the built-in Sony apps. Any mechanical shutter operation and sensor readout consumes the bulk of this time.

### Is 500 ms possible from image-available-to-app?
**Verdict:** **Conditional (Yes for proxy/preview).**
Once the image data is available in memory to the PMCA Android layer, 500 ms is achievable *only* if the resolution is reduced and memory allocations are eliminated.
If we attempt to decode, process, and re-encode a 42 MP JPEG (approx 10-15 MB size) in Android Java or even JNI C++, the memory bandwidth and CPU speed of the older A7R II processor will exceed 500 ms.

### Realistic Resolution
To guarantee < 500 ms processing from the moment the Android layer receives the image:
*   **Fastest:** 640 x 480 (VGA) preview stream - < 50 ms
*   **Balanced:** 1280 x 720 (720p) proxy - < 150 ms
*   **High:** 1920 x 1080 (1080p) proxy - ~300-400 ms
*   **Original (42 MP):** > 2000 ms (Not recommended for synchronous UI).

### Operations that must be avoided:
1.  **RAW Demosaicing:** The app must request YUV or JPEG from the Camera API.
2.  **Floating-point math per pixel:** BIONZ X CPU is not optimized for heavy FP SIMD in the Android space. Fixed-point integer math is mandatory.
3.  **Large 3D LUTs:** Interpolating a 33x33x33 or 64x64x64 3D LUT per pixel will bust the cache and exceed the budget.
4.  **Per-frame Memory Allocation:** Garbage collection in Android 4.x Dalvik will cause massive stutters. Memory pools are required.

### Required Sony SDK Capabilities
1.  `com.sony.scalar.hardware.CameraEx` - To access extended camera parameters, specifically capturing proxy resolutions while retaining full-resolution capabilities if running async.
2.  Hardware JPEG Decoder/Encoder or direct YUV buffer access (e.g., `setPreviewCallback` with `NV21` format) to bypass JPEG decoding entirely.

### What happens if those capabilities are missing?
If we cannot hook into `NV21` preview callbacks or hardware-accelerated proxy JPEGs, we must fallback to taking a standard JPEG, downsampling it via Android `BitmapFactory.Options.inSampleSize`, and processing it. This pushes 1080p processing dangerously close to or slightly over the 500 ms limit due to software decoding overhead.
