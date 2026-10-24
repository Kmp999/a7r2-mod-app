# Timing Budget (500 ms Target)

## Overview
This budget targets 500 ms from **image available to app** to **save initiated**.
Total time from physical shutter press is excluded as the mechanical/readout time of a 42MP sensor is fixed by Sony hardware and exceeds this budget.

Resolution target: **1280 x 720 (Proxy JPEG / NV21)**

| Stage | Estimated ms | Measured ms | Pass/Fail Threshold | Optimization Fallback |
| :--- | :--- | :--- | :--- | :--- |
| `trigger_pressed` | 0 | `[ ]` | N/A | N/A |
| `autofocus_exposure_wait` | (Sony HW) | `[ ]` | N/A | Manual Focus / AE Lock |
| `sensor_readout` | (Sony HW) | `[ ]` | N/A | N/A |
| `camera_jpeg_encode` | (Sony HW) | `[ ]` | N/A | Request NV21 instead of JPEG |
| `image_available_to_app` | 0 | `[ ]` | N/A | N/A |
| `app_read_image` | 10 | `[ ]` | < 20 ms | Pre-allocate JNI ByteBuffers |
| `app_decode_image` (if JPEG) | 80 | `[ ]` | < 120 ms| Use NV21 Preview Callback directly |
| `app_downsample_if_needed` | 30 | `[ ]` | < 50 ms | Nearest-neighbor scaling in C++ |
| `app_apply_color_matrix` | 60 | `[ ]` | < 100 ms| Fixed-point Q8.8 math only |
| `app_apply_tone_curve_lut` | 40 | `[ ]` | < 80 ms | Combine Matrix+LUT into 1 pass |
| `app_optional_saturation` | 20 | `[ ]` | < 40 ms | Combine with Matrix pass or disable |
| `app_encode_output` (JPEG) | 120 | `[ ]` | < 150 ms| libjpeg-turbo optimized with SIMD |
| `app_save_output` (Async) | 50 | `[ ]` | < 100 ms| Fire-and-forget background thread |
| `app_ui_update` (Async) | 5 | `[ ]` | < 20 ms | Update basic text view only |

**Total Estimated:** ~415 ms (Budget: 500 ms)

## Bottleneck Mitigations
1. **If Decode is > 120ms:** Abandon JPEG, strictly hook into `Camera.PreviewCallback` for NV21 YUV data.
2. **If Color Matrix is > 100ms:** Fallback to 1D LUTs only (no cross-channel matrix).
3. **If Encode is > 150ms:** Reduce JPEG quality to 80-85% instead of 95-100%.
