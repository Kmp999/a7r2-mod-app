#!/usr/bin/env python3
"""
Unit Test Suite for Three Field-Hardening Architectural Enhancements:
  1. ARGB_8888 High-Precision Strip Decoding with RGB_565 OOM fallback (Sky posterization elimination).
  2. SD Card I/O Contention Mitigation: pendingJobsCount (AtomicInteger), MAX_PENDING_JOBS=8 bounding,
     settling backoff in waitForJpegComplete, and [GRADING: N] live HUD indicator.
  3. Graceful Shutdown & Atomic .tmp Flush: awaitTermination(2500ms), isShuttingDown mid-stream abort,
     and cleanupOrphanedTempFiles() startup sweep.
"""

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

class TestEnhancementsPrecisionQueueDrain(unittest.TestCase):
    def setUp(self):
        self.src_dir = REPO_ROOT / "src"
        self.capture_file = self.src_dir / "app" / "CaptureController.java"
        self.storage_file = self.src_dir / "app" / "StorageResolver.java"
        self.ui_file = self.src_dir / "app" / "UIController.java"
        self.qmenu_file = self.src_dir / "app" / "FujiQMenuView.java"
        self.color_engine_java = self.src_dir / "color" / "ColorEngine.java"
        self.color_jni_c = self.src_dir / "color" / "color_engine_jni.c"
        self.lut_h = self.src_dir / "color" / "lut_cube_17.h"
        self.jpeg_writer_c = self.src_dir / "image" / "native_jpeg_writer.c"
        self.jpeg_writer_h = self.src_dir / "image" / "native_jpeg_writer.h"

    # ------------------------------------------------------------------------
    # Enhancement 1: ARGB_8888 Precision & OOM Fallback
    # ------------------------------------------------------------------------
    def test_argb_8888_config_and_oom_fallback(self):
        """Enhancement 1A: CaptureController requests ARGB_8888 and falls back to RGB_565 on OOM."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("opts.inPreferredConfig = Bitmap.Config.ARGB_8888;", content)
        self.assertIn("catch (OutOfMemoryError oom)", content)
        self.assertIn("opts.inPreferredConfig = Bitmap.Config.RGB_565;", content)
        self.assertIn("stripBmp = decoder.decodeRegion(rect, opts);", content)

    def test_argb_8888_buffer_budget_under_4mb(self):
        """Enhancement 1B: Strip buffer allocation is 3976 * 64 * 4 (~1.01 MB), strictly within <4MB budget."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("java.nio.ByteBuffer.allocateDirect(dstW * 64 * 4)", content)
        # Immediate recycle ensures memory is returned to runtime
        self.assertIn("stripBmp.recycle()", content)

    def test_native_argb_8888_lut_and_jpeg_writer(self):
        """Enhancement 1C: C engine and JNI provide full 8-bit ARGB_8888 grading and JPEG streaming."""
        lut_content = self.lut_h.read_text(encoding="utf-8")
        writer_h = self.jpeg_writer_h.read_text(encoding="utf-8")
        writer_c = self.jpeg_writer_c.read_text(encoding="utf-8")
        jni_content = self.color_jni_c.read_text(encoding="utf-8")

        self.assertIn("lut17_process_argb8888_advanced_offset", lut_content)
        self.assertIn("native_jpeg_stream_write_strip_argb8888", writer_h)
        self.assertIn("native_write_jpeg_strip_argb8888", writer_h)
        self.assertIn("native_jpeg_stream_write_strip_argb8888", writer_c)
        self.assertIn("lut17_process_argb8888_advanced_offset", jni_content)
        self.assertIn("native_jpeg_stream_write_strip_argb8888", jni_content)

    # ------------------------------------------------------------------------
    # Enhancement 2: SD Card I/O Queue Counter & Bounding
    # ------------------------------------------------------------------------
    def test_pending_jobs_count_and_queue_bounding(self):
        """Enhancement 2A: CaptureController bounds queue to MAX_PENDING_JOBS = 8 with AtomicInteger."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("MAX_PENDING_JOBS = 8", content)
        self.assertIn("AtomicInteger pendingJobsCount", content)
        self.assertIn("pendingJobsCount.incrementAndGet()", content)
        self.assertIn("pendingJobsCount.decrementAndGet()", content)
        self.assertIn("getPendingJobsCount()", content)
        # Hardware capture continues when queue is full
        self.assertIn("StorageResolver.saveRawJpegToSd(context, jpegData, seqNum);", content)

    def test_io_settling_pacing_in_wait_for_jpeg(self):
        """Enhancement 2B: StorageResolver waits with non-aggressive settling backoff after EOI."""
        storage_content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("waitForJpegComplete", storage_content)
        self.assertIn("Thread.sleep(40)", storage_content)
        self.assertIn("Thread.sleep(35)", storage_content)

    def test_live_hud_grading_badge(self):
        """Enhancement 2C: UIController and FujiQMenuView render [GRADING: N] HUD badge."""
        ui_content = self.ui_file.read_text(encoding="utf-8")
        qmenu_content = self.qmenu_file.read_text(encoding="utf-8")

        self.assertIn("setPendingGradingCount", ui_content)
        self.assertIn("getPendingGradingCount", ui_content)
        self.assertIn("setPendingGradingCount", qmenu_content)
        self.assertIn("hudGradingBadgePaint", qmenu_content)
        self.assertIn('[GRADING: "', qmenu_content)

    # ------------------------------------------------------------------------
    # Enhancement 3: Graceful Shutdown & .tmp Cleanup
    # ------------------------------------------------------------------------
    def test_graceful_shutdown_drain(self):
        """Enhancement 3A: CaptureController.shutdown() awaits termination with 2500ms timeout."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("isShuttingDown = true", content)
        self.assertIn("awaitTermination(2500, java.util.concurrent.TimeUnit.MILLISECONDS)", content)
        self.assertIn("processingExecutor.shutdownNow()", content)

    def test_mid_stream_abort_and_tmp_deletion(self):
        """Enhancement 3B: Strip loop checks isShuttingDown and deletes incomplete .tmp files on abort."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("if (isShuttingDown)", content)
        self.assertIn("tmpFile.delete()", content)

    def test_startup_orphan_sweep(self):
        """Enhancement 3C: StorageResolver.cleanupOrphanedTempFiles() cleans .tmp files on startup."""
        storage_content = self.storage_file.read_text(encoding="utf-8")
        capture_content = self.capture_file.read_text(encoding="utf-8")

        self.assertIn("cleanupOrphanedTempFiles", storage_content)
        self.assertIn('endsWith(".tmp")', storage_content)
        self.assertIn("StorageResolver.cleanupOrphanedTempFiles();", capture_content)

if __name__ == '__main__':
    unittest.main()
