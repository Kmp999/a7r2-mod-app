# Build & Deployment Instructions for Sony A7R II

## 1. Environment Requirements
*   **Host OS:** Windows 10/11 or Linux
*   **Android NDK:** r10e (Legacy toolchain targeting ARMv7-A / Android API Level 10-14)
*   **Android SDK:** Build tools with API 10 or 15 support
*   **PMCA Tool:** `pmca-gui-v0.18-win.exe` or `pmca-console` (open-source Sony PMCA loader)
*   **Camera:** Sony Alpha 7R II (ILCE-7RM2) with firmware 3.xx or 4.xx

---

## 2. Compiling the Native Color Engine (NDK)
1. Open terminal and navigate to the project jni directory:
   ```bash
   cd fuji-lite-a7r2/jni
   ```
2. Execute `ndk-build`:
   ```bash
   ndk-build
   ```
   This generates `libs/armeabi-v7a/libcolor_engine_jni.so` compiled with `-O3 -mfpu=neon -ftree-vectorize`.

---

## 3. Packaging the APK
1. Compile the Java source files in `src/` against Android API Level 10/14.
2. Package into `fuji-lite-a7r2.apk` containing:
   - `classes.dex`
   - `lib/armeabi-v7a/libcolor_engine_jni.so`
   - `AndroidManifest.xml` (targeting `android.minSdkVersion=10`)

---

## 4. Deploying to Sony A7R II
1. Power on your Sony A7R II camera.
2. Open Camera Menu -> **Setup (Toolbox)** -> **USB Connection** -> Set to **Mass Storage** or **MTP**.
3. Connect the camera to your computer using a micro-USB data cable.
4. Launch `pmca-gui`:
   - Wait for the camera to be recognized.
   - Switch to the **Install** tab.
   - Click **Select an apk** and pick `fuji-lite-a7r2.apk`.
   - Click **Install**.
5. Once complete, safely eject the camera.
6. On the camera, open **Menu -> Application -> Application List -> Fuji Lite**.
