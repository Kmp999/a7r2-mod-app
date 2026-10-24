# Risk Analysis

## High Risks
1. **Sony PMCA SDK Unavailable / Obsolete:** OpenMemories framework is community-maintained. If it breaks on A7R II firmware v3.30+, the app cannot be installed or access camera hardware.
2. **Cannot Access Captured Image Pixels Quickly:** The A7R II is a 42 MP camera. If we request a standard `PictureCallback`, we might be forced to receive a 42 MP JPEG. Decoding this in Android Dalvik VM (or even C++) will take seconds and crash the app with `OutOfMemoryError`.
3. **JPEG Decode / Encode Too Slow:** If `NV21` preview frames are not high enough quality and we must rely on JPEG proxies, the libjpeg decoding/encoding step might exceed 150ms per frame.
4. **Thermal Throttling:** Running custom image processing heavily utilizes the main CPU, not the dedicated BIONZ X ISP. The camera might overheat and shut down in continuous use.

## Medium Risks
1. **Storage Write Speed:** Writing to the SD Card from the Dalvik VM is subject to Android file system overhead. Synchronous writes will freeze the app. Must use background threads.
2. **UI Thread Blocked:** Older Android versions are very sensitive to UI thread blocking. Any image processing on the main thread will trigger an ANR (Application Not Responding) dialog.
3. **Memory Limits Exceeded:** The A7R II Android subsystem has strict heap limits (often 32MB or 64MB). Allocating large bitmaps will instantly crash. Memory pooling in JNI is strictly required.

## Low Risks
1. **Camera Cannot Expose Color Pipeline:** If `CameraEx` cannot modify hardware color, we fallback to our software JNI pipeline. We already planned for this.
2. **App Signing Restrictions:** pmca-gui bypasses the official store. User must be willing to use it.
