# Fuji Lite for Sony A7R II

This is a specialized, ultra-fast image processing application designed to run **natively inside the Sony A7R II** via the PMCA (PlayMemories Camera Apps) Android subsystem.

## Project Purpose
To provide Fujifilm-inspired color treatments (Provia, Astia, Velvia, Classic Chrome/Neg, Eterna approximations) directly on the camera, allowing straight-out-of-camera (SOOC) workflows without a smartphone or PC.

**Legal Note:** The presets included in this application are original mathematical approximations inspired by film aesthetics. They do not contain, reverse-engineer, or reproduce proprietary Fujifilm color science data or LUTs.

## Supported Modes
1.  **JPEG Proxy Mode (Default & Fastest):** Captures the camera's YUV NV21 preview buffer (typically 720p or 1080p), applies a fixed-point color matrix and 1D LUT in native C++ JNI, and saves the result to the SD card.
2.  *(Future)* **Internal Pipeline Mode:** If `com.sony.scalar` APIs allow deep hardware manipulation of the picture profiles, we will bypass software processing entirely.

## Build Instructions (Prerequisites)
1.  **Android NDK (r10e or similar old version):** Required to compile the C++ JNI code for the older `armeabi-v7a` Android 2.3/4.x system on the camera.
2.  **Android SDK (API Level 10-15):** Required to compile the Java wrapper.
3.  **Apache Ant / Gradle:** Depending on your preferred Android build wrapper.

### Compilation
1.  Navigate to `./fuji-lite-a7r2`
2.  Run `ndk-build` to compile the `color_engine_jni.so` library.
3.  Compile the Android APK using `ant release` or Gradle targeting API 10.

## Deployment (Sony A7R II)
1.  Download `pmca-gui` (https://github.com/ma1co/Sony-PMCA-RE)
2.  Connect your A7R II via USB in **MTP or Mass Storage** mode.
3.  Open `pmca-gui`, go to the "Install" tab.
4.  Select the compiled `fuji-lite-a7r2.apk` and install it.
5.  Disconnect USB, go to the camera's "Application List", and launch "Fuji Lite".

## Known Limitations
*   **No Full 42MP RAW Processing:** Processing a 42MP image inside the camera's Dalvik VM would take seconds and likely crash due to OutOfMemory errors. The app works on proxy/preview resolutions (720p/1080p) for speed.
*   **UI is Minimal:** To guarantee the <500ms processing budget and prevent UI thread locking, the interface is purely functional text and basic buttons.
*   **Sony API Restrictions:** The full capability of the internal ISP is locked behind undocumented `com.sony.scalar` APIs.

## Architecture
See `ARCHITECTURE.md` for a full system breakdown of the UI, JNI bridge, and memory pooling model.
See `FEASIBILITY.md` and `TIMING_BUDGET.md` for the performance engineering metrics.
