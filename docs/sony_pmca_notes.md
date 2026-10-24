# Sony PMCA (PlayMemories Camera Apps) Technical Notes

## 1. Operating Environment on Sony Alpha 7R II (ILCE-7RM2)
- **OS Subsystem:** Android 4.1.2 Jelly Bean (API Level 16) running on a dual-core ARM Cortex processor coupled with Sony's BIONZ X Image Processing Engine.
- **Display System:** 640x480 RGB VGA LCD panel (or OLED EVF). Screen surface buffers are generally 640x480.
- **Java Virtual Machine:** Dalvik VM. Strict GC pauses occur if objects are allocated inside callback loops. Heap size limit per application is configured between 32MB and 64MB.

## 2. Hardware vs Subsystem Boundary
The camera has two primary computing environments:
1. **Camera Core / ISP (BIONZ X hardware):** Controls sensor capture, phase-detection autofocus (399 AF points), electronic front curtain shutter, mechanical shutter, and hardware JPEG/RAW encoding.
2. **Application Processor (AP / Linux / Android):** Hosts user interface, network stack (Wi-Fi), and PMCA apps.

```
[ Sensor 42.4 MP ] ---> [ BIONZ X ISP ] ---> [ Hardware RAW/JPEG ]
                                |                        |
                         (Live View Bus)          (Storage Bus)
                                |                        |
                         [ V4L2 Driver ]                 |
                                |                        |
                         [ Android HAL ]                 |
                                |                        |
                    [ android.hardware.Camera ]          |
                    [ com.sony.scalar.hardware]          |
                                |                        v
                         [ PMCA App JNI ] -------> [ SD Card / DCIM ]
```

## 3. Communication and Control
- Standard Android `android.hardware.Camera` provides basic preview frames (`PreviewCallback`) and picture capture (`PictureCallback`).
- Extended Sony library `com.sony.scalar.hardware.CameraEx` provides camera-specific controls:
  - Shutter speed, Aperture, ISO control.
  - Focus drive and AF status.
  - Picture Profile control (`CameraEx.ParametersModifier`).
  - Auto-review suppression (`CameraEx.AutoPictureReviewControl`).

## 4. Bottleneck Analysis for 42 MP Full Frame Processing
- A 42.4 MP RGBA bitmap requires `7952 * 5304 * 4 = 168,746,496 bytes (~161 MB)`.
- The Dalvik heap limit is ~64 MB. Allocating a single 42 MP uncompressed bitmap crashes Dalvik with an unrecoverable `OutOfMemoryError`.
- Even in native C/JNI, copying 161 MB over memory buses on a mobile SoC of that era requires > 300 ms purely for memory transfers, leaving virtually no time for color computation.
- Therefore, real-time / sub-500ms processing **must** operate on downscaled proxies (e.g. 1280x720 or 1920x1080) or direct YUV stream buffers.
