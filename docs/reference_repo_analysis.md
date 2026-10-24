# Reference Repository Analysis: LightLab-sony-pcma

## 1. Overview & Repository Contents
- **Target Repository:** `https://github.com/haiha310390-HN/LightLab-sony-pcma.git`
- **Cloned Artifacts:**
  - `LightLab_a7R_v52.apk`
  - `LightLab_a7R_v54_for Full Frame camera.apk`
  - `LightLab_NEX_v55 for NEX 5R 5T 6.apk`
  - `pmca-gui-v0.18-win.exe`
  - `CUBE.rar` & `CUBE for sony nex 5R 5T 6.rar` (3D LUT CUBE files)
  - `README.md` (Vietnamese/English installation guide via pmca-gui)

## 2. Platform & Environment Architecture
1. **Underlying Runtime:** Android OS (specifically Android 2.3.7 Gingerbread to Android 4.1.2 Jelly Bean depending on camera generation).
2. **Subsystem:** Sony PlayMemories Camera Apps (PMCA) platform running atop a custom Linux kernel on Sony BIONZ X / BIONZ imaging engines.
3. **Execution Model:** Dalvik VM. Apps are standard Android `.apk` files signed or installed via Sony service mode / PMCA debug interface through `pmca-gui` (reverse engineered by `ma1co` and community).
4. **LUT Mechanism in LightLab:**
   - LightLab relies on reading external 3D `.cube` LUT files from `/sdcard/CUBE/` on the memory card.
   - Processing full 3D LUTs inside Dalvik on an ARM Cortex-A8 / Cortex-A9 single/dual-core CPU embedded inside the A7R II causes notable latency, often several seconds per shot, which violates real-time shooting requirements.

## 3. Discovered Bottlenecks & Critical Flaws in Reference Design
1. **Heavy 3D LUT Interpolation:** Trilinear interpolation on 33x33x33 or 64x64x64 cubes per pixel in Dalvik or unoptimized native code severely degrades performance.
2. **Synchronous SD Card Access:** Reading `.cube` files from FAT32 SD card on every frame or shot blocks the UI/capture pipeline.
3. **Memory Pressure:** Loading full 24MP/42MP bitmaps causes immediate `OutOfMemoryError` due to camera app heap limits (typically 32MB - 64MB).
4. **Lack of Internal ISP Interfacing:** LightLab performs post-processing on captured images rather than leveraging the hardware pipeline or lightweight fixed-point streaming.

## 4. Architectural Directives for fuji-lite-a7r2
- **Eliminate 3D LUTs at runtime:** Replace with 3x3 fixed-point color matrices + three 1D tone-curve LUTs (256 bytes each).
- **Zero dynamic allocations:** Pre-allocate static working buffers in a unified memory pool.
- **Fixed-point arithmetic:** Strictly use integer/Q8.8 math. No floating-point operations in the per-pixel inner loop.
- **Process proxy/preview streams:** Target 720p/1080p proxy buffers to guarantee sub-500ms turnaround from `image_available_to_app`.
- **Decoupled Architecture:** Clean C core for color math, abstracted Camera Adapter for Sony PMCA vs Mock/Host test harnesses.
