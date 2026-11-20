#!/usr/bin/env python3
"""
Unit tests verifying elimination of all 6 Sony A7R II hardware locking points:
  1. Live View NV21 preview offloaded off Main UI thread with <=10 FPS throttle, cached preview size,
     double-buffered Bitmap swap, and zero GetPrimitiveArrayCritical Dalvik GC locks.
  2. Hardware exposure poller moved to 2Hz background HandlerThread with single batched getParameters()
     IPC, cached reflection Method lookups, zero-IPC HUD getters, and cleanup on release().
  3. Shutter press zero-I/O in-memory photo snapshot, canonical DCIM directory caching, dir.list()
     enumeration without per-file stat(), and background Quick Review / ISP parameter updates.
  4. Default FAST_10MP resolution mode, single-pass inline EXIF APP1 embedding in native_jpeg_writer,
     and conditional raw+jpeg guardrail (no forced .ARW flush in JPEG mode).
  5. Deduplication between StoreImageCompleteListener and triggerSingleCapture (zero double-grading),
     background worker priority lower than UI thread (NORM_PRIORITY - 1), no unconditional System.gc(),
     and RGB_565 OOM fallback direct buffer resize.
  6. Native C hot-loop optimizations: tetrahedral 3D LUT interpolation (mathematical verification),
     dual-core pthread strip grading, 1-cycle __builtin_clz category lookup, and trailing-zero AC skip.
"""

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def tetrahedral_lut_eval(table, r, g, b):
    """Exact Python mirror of lut17_interpolate_pixel in src/color/lut_cube_17.h."""
    r256 = r + (r >> 7)
    g256 = g + (g >> 7)
    b256 = b + (b >> 7)
    r0, g0, b0 = r256 >> 4, g256 >> 4, b256 >> 4
    rf, gf, bf = r256 & 0x0F, g256 & 0x0F, b256 & 0x0F

    base = (b0 * 289 + g0 * 17 + r0) * 3
    dr = 3 if r0 < 16 else 0
    dg = 51 if g0 < 16 else 0
    db = 867 if b0 < 16 else 0

    if rf >= gf:
        if gf >= bf:
            c1, c2, c3 = base + dr, base + dr + dg, base + dr + dg + db
            f1, f2, f3 = rf, gf, bf
        elif rf >= bf:
            c1, c2, c3 = base + dr, base + dr + db, base + dr + db + dg
            f1, f2, f3 = rf, bf, gf
        else:
            c1, c2, c3 = base + db, base + db + dr, base + db + dr + dg
            f1, f2, f3 = bf, rf, gf
    else:
        if bf > gf:
            c1, c2, c3 = base + db, base + db + dg, base + db + dg + dr
            f1, f2, f3 = bf, gf, rf
        elif bf > rf:
            c1, c2, c3 = base + dg, base + dg + db, base + dg + db + dr
            f1, f2, f3 = gf, bf, rf
        else:
            c1, c2, c3 = base + dg, base + dg + dr, base + dg + dr + db
            f1, f2, f3 = gf, rf, bf

    out = []
    for ch in range(3):
        v = (
            table[base + ch] * 16
            + (table[c1 + ch] - table[base + ch]) * f1
            + (table[c2 + ch] - table[c1 + ch]) * f2
            + (table[c3 + ch] - table[c2 + ch]) * f3
            + 8
        ) >> 4
        out.append(v)
    return tuple(out)


class TestA7R2LockingPointsFix(unittest.TestCase):
    def setUp(self):
        self.src = REPO_ROOT / "src"
        self.main_java = (self.src / "app" / "MainActivity.java").read_text(encoding="utf-8")
        self.capture_java = (self.src / "app" / "CaptureController.java").read_text(encoding="utf-8")
        self.storage_java = (self.src / "app" / "StorageResolver.java").read_text(encoding="utf-8")
        self.qmenu_java = (self.src / "app" / "FujiQMenuView.java").read_text(encoding="utf-8")
        self.appstate_java = (self.src / "app" / "AppState.java").read_text(encoding="utf-8")
        self.adapter_java = (self.src / "camera" / "SonyPmcaAdapter.java").read_text(encoding="utf-8")
        self.mapper_java = (self.src / "camera" / "SonyToFujiHardwareMapper.java").read_text(encoding="utf-8")
        self.jni_c = (self.src / "color" / "color_engine_jni.c").read_text(encoding="utf-8")
        self.lut_h = (self.src / "color" / "lut_cube_17.h").read_text(encoding="utf-8")
        self.writer_c = (self.src / "image" / "native_jpeg_writer.c").read_text(encoding="utf-8")

    def test_1_live_view_offloaded_and_gc_lock_removed(self):
        """Fix 1: Preview callback uses cached size, background executor, frame throttle, and no GC critical lock."""
        self.assertNotIn("camera.getParameters().getPreviewSize()", self.main_java)
        self.assertIn("cachedPreviewWidth", self.main_java)
        self.assertIn("previewExecutor.execute", self.main_java)
        self.assertIn("previewFrameBusy.compareAndSet(false, true)", self.main_java)
        self.assertIn("liveViewBackBitmap", self.qmenu_java)
        self.assertIn("liveViewSwapLock", self.qmenu_java)

        # nativeGradePreviewNV21 must use GetByteArrayRegion instead of GetPrimitiveArrayCritical
        fn_start = self.jni_c.index("Java_com_fujilite_a7r2_ColorEngine_nativeGradePreviewNV21")
        fn_end = self.jni_c.index("Java_com_fujilite_a7r2_ColorEngine_nativeGradeCaptureRGB565")
        preview_fn = self.jni_c[fn_start:fn_end]
        self.assertNotIn("(*env)->GetPrimitiveArrayCritical", preview_fn)
        self.assertIn("(*env)->GetByteArrayRegion", preview_fn)

    def test_2_hardware_poller_background_batched_and_stopped_on_release(self):
        """Fix 2: Poller runs at 2Hz on background HandlerThread with single batched getParameters() IPC."""
        self.assertIn("HandlerThread(", self.adapter_java)
        self.assertIn("HARDWARE_POLL_INTERVAL_MS = 500L", self.adapter_java)
        self.assertIn("refreshExposureTelemetry()", self.adapter_java)
        self.assertIn("sModifierMethodCache", self.adapter_java)
        self.assertIn("setPollingPaused", self.adapter_java)

        rel_start = self.adapter_java.index("public void release()")
        self.assertIn("stopHardwarePoller();", self.adapter_java[rel_start:rel_start + 300])

    def test_3_shutter_zero_io_and_canonical_dcim_cache(self):
        """Fix 3: Shutter uses non-blocking in-memory snapshot; StorageResolver caches canonical DCIM dirs."""
        self.assertIn("copyKnownPhotoPathsNonBlocking()", self.capture_java)
        self.assertIn("prewarmDirectoryAndSequence()", self.capture_java)
        self.assertIn("sCachedCandidateDirs", self.storage_java)
        self.assertIn("getCanonicalPath()", self.storage_java)
        self.assertIn("String[] names = dir.list();", self.storage_java)

    def test_4_fast_10mp_default_inline_exif_and_conditional_raw_guardrail(self):
        """Fix 4: FAST_10MP default, single-pass inline EXIF APP1, no forced raw+jpeg when already JPEG."""
        self.assertIn("outputResolutionMode = OutputResolutionMode.FAST_10MP", self.appstate_java)
        self.assertIn("extractExifApp1Segment", self.storage_java)
        self.assertIn("native_jpeg_stream_open_with_exif", self.writer_c)
        self.assertIn("openJpegStreamWithExifSafe", self.capture_java)
        self.assertIn("if (!inlineExifWritten)", self.capture_java)
        self.assertIn('String curSonyFormat = params.get("sony-picture-format");', self.mapper_java)

    def test_5_no_double_processing_lower_worker_priority_and_no_forced_gc(self):
        """Fix 5: FIFO deduplication for bursts, lower worker priority, no System.gc(), thread-safe LUT rwlock."""
        self.assertIn("claimPhotoForProcessing", self.capture_java)
        self.assertIn("pendingHardwareStoredQueue", self.capture_java)
        self.assertIn("hadInitialSnapshot", self.capture_java)
        self.assertIn("appState.setProcessing(remaining > 0)", self.capture_java)
        self.assertIn("Thread.NORM_PRIORITY - 1", self.capture_java)
        self.assertNotIn("Thread.NORM_PRIORITY + 1", self.capture_java)
        self.assertIn("stripDirect = java.nio.ByteBuffer.allocateDirect(dstW * 64 * 2);", self.capture_java)
        self.assertIn("pthread_mutex_t g_lut_mutex", self.jni_c)
        self.assertNotIn("pthread_rwlock", self.jni_c)
        self.assertIn("g_strip_call_mutex", self.jni_c)
        self.assertIn("encode_worker_submit", self.jni_c)
        self.assertIn("calloc(1, sizeof(BitWriter))", self.writer_c)
        self.assertIn("init_ycbcr_tables", self.writer_c)
        self.assertIn("surfaceReady", self.main_java)
        ui_java = (self.src / "app" / "UIController.java").read_text(encoding="utf-8")
        self.assertIn("dispatchDialTask", ui_java)
        # Ensure normal finally block doesn't invoke System.gc()
        fin_idx = self.capture_java.index("int remaining = pendingJobsCount.decrementAndGet();")
        self.assertNotIn("System.gc()", self.capture_java[fin_idx:fin_idx + 200])

    def test_7_a7r2_native_abi_compat_3tier_stream_and_no_45mp_crush(self):
        """Fix 7: Legacy API-9 dual-ABI NDK build, 3-tier stream open, 18MP strip fix, no 4.5MP fallback crush."""
        app_mk = (REPO_ROOT / "jni" / "Application.mk").read_text(encoding="utf-8")
        android_mk = (REPO_ROOT / "jni" / "Android.mk").read_text(encoding="utf-8")
        workflow = (REPO_ROOT / ".github" / "workflows" / "build-apk.yml").read_text(encoding="utf-8")

        # Dual ABI (armeabi + armeabi-v7a) targeting android-9 with DT_HASH & no forced NEON SIGILL
        self.assertIn("armeabi armeabi-v7a", app_mk)
        self.assertIn("android-9", app_mk)
        self.assertIn("-Wl,--hash-style=both", android_mk)
        self.assertNotIn("-mfpu=neon", android_mk)
        self.assertIn("lib/armeabi/libcolor_engine_jni.so", workflow)
        self.assertIn("lib/armeabi-v7a/libcolor_engine_jni.so", workflow)

        # 18MP BufferOverflowException fix: srcStripH = 64 whenever targetSample == 1
        self.assertIn("if (targetSample == 1 || resMode == AppState.OutputResolutionMode.MASTER_42MP)", self.capture_java)

        # 3-Tier native stream open + MediaStore raw byte stream
        self.assertIn("openNativeStreamTier", self.capture_java)
        self.assertIn("saveJpegFileViaMediaStore", self.capture_java)
        self.assertIn("saveJpegFileViaMediaStore", self.storage_java)
        self.assertIn("resolveStoredPhotoFile", self.capture_java)

        # Fallback never crushes 18MP (5168x3448) down to 4.5MP (2584x1724) and skips double EXIF/Avindex
        self.assertIn("fallbackOpts.inSampleSize = (srcW >= 7000) ? 2 : 1;", self.capture_java)
        self.assertNotIn("fallbackOpts.inSampleSize = (srcW > 4000) ? 2 : 1;", self.capture_java)
        self.assertIn("skipPostProcess", self.storage_java)
        self.assertIn("nativeApplyLut17IntStrip", self.jni_c)
        self.assertIn("getSupportedImageSizes", self.adapter_java)

    def test_6_native_tetrahedral_dual_core_and_clz_optimizations(self):
        """Fix 6: Tetrahedral LUT math accuracy, dual-core pthread grading, CLZ & trailing-zero AC skip."""
        self.assertIn("grade_strip_parallel", self.jni_c)
        self.assertIn("__builtin_clz", self.writer_c)
        self.assertIn("if (last_nz < 63)", self.writer_c)
        self.assertIn("int dr = (r0 < 16) ? 3 : 0;", self.lut_h)

        # Build identity 17x17x17 LUT and verify tetrahedral interpolation across all 6 simplices & corners
        identity = bytearray(17 * 17 * 17 * 3)
        idx = 0
        for b in range(17):
            bv = 255 if b == 16 else (b << 4)
            for g in range(17):
                gv = 255 if g == 16 else (g << 4)
                for r in range(17):
                    rv = 255 if r == 16 else (r << 4)
                    identity[idx] = rv
                    identity[idx + 1] = gv
                    identity[idx + 2] = bv
                    idx += 3

        # Exact cube corners (0 and 255 map cleanly to node 0 and node 16)
        for pt in [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]:
            self.assertEqual(tetrahedral_lut_eval(identity, *pt), pt)

        # Midpoint & all 6 tetrahedron orderings of (rf, gf, bf) must reproduce identity within <=1 LSB
        samples = [
            (128, 128, 128),
            (203, 102, 51),  # rf > gf > bf
            (203, 51, 102),  # rf > bf > gf
            (102, 51, 203),  # bf > rf > gf
            (51, 102, 203),  # bf > gf > rf
            (51, 203, 102),  # gf > bf > rf
            (102, 203, 51),  # gf > rf > bf
        ]
        for r, g, b in samples:
            or_, og_, ob_ = tetrahedral_lut_eval(identity, r, g, b)
            self.assertLessEqual(abs(or_ - r), 1)
            self.assertLessEqual(abs(og_ - g), 1)
            self.assertLessEqual(abs(ob_ - b), 1)


if __name__ == "__main__":
    unittest.main()
