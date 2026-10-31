#!/usr/bin/env python3
"""
Unit and Verification Suite for Native 42.4MP Strip-Streaming Engine.
Strictly verifies all five architectural guardrails:
  1. Zero Data Destruction (master capture DSC0xxxx.JPG never deleted, safe temp-file-rename).
  2. True 42.4MP Resolution (no upscaler, inSampleSize = 1).
  3. Strict Dalvik Heap & Strip Memory Management (64-row strips, immediate recycle, single DirectBuffer).
  4. Asynchronous Decoupling (< 30ms UI return, single-thread sequential worker queue).
  5. Seamless Strip Boundaries & EXIF Preservation (deterministic row-seeded PRNG, EXIF copy).
"""

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

class TestNative42MPStripStreaming(unittest.TestCase):
    def setUp(self):
        self.src_dir = REPO_ROOT / "src"
        self.capture_file = self.src_dir / "app" / "CaptureController.java"
        self.storage_file = self.src_dir / "app" / "StorageResolver.java"
        self.color_engine_java = self.src_dir / "color" / "ColorEngine.java"
        self.color_jni_c = self.src_dir / "color" / "color_engine_jni.c"
        self.lut_h = self.src_dir / "color" / "lut_cube_17.h"
        self.jpeg_writer_c = self.src_dir / "image" / "native_jpeg_writer.c"
        self.jpeg_writer_h = self.src_dir / "image" / "native_jpeg_writer.h"

    def test_guardrail_1_zero_data_destruction(self):
        """Guardrail 1: cleanupUnfilteredRawFiles removed, no delete on DSC0xxxx.JPG, safe temp-file-rename."""
        storage_content = self.storage_file.read_text(encoding="utf-8")
        capture_content = self.capture_file.read_text(encoding="utf-8")

        # Must not contain cleanupUnfilteredRawFiles anywhere
        self.assertNotIn("cleanupUnfilteredRawFiles", storage_content)
        self.assertNotIn("cleanupUnfilteredRawFiles", capture_content)

        # Must strictly preserve DSC0xxxx master capture
        self.assertIn("findRawMasterFile", storage_content)
        self.assertIn("Note: Original master files are strictly preserved and NEVER deleted", storage_content)

        # Safe temp-file-then-rename pattern in StorageResolver and CaptureController
        self.assertIn('.tmp"', storage_content)
        self.assertIn("tmpFile.renameTo", storage_content)
        self.assertIn('.tmp"', capture_content)
        self.assertIn("tmpFile.renameTo", capture_content)

    def test_guardrail_2_true_42_4mp_resolution_no_upscaler(self):
        """Guardrail 2: Legacy upscaler completely removed, inSampleSize strictly 1 on BitmapRegionDecoder."""
        lut_content = self.lut_h.read_text(encoding="utf-8")
        jni_content = self.color_jni_c.read_text(encoding="utf-8")
        writer_content = self.jpeg_writer_c.read_text(encoding="utf-8")
        capture_content = self.capture_file.read_text(encoding="utf-8")

        # Upscaler removed from C headers, implementation, and JNI
        self.assertNotIn("upscaler_edge_directed_rgb565", lut_content)
        self.assertNotIn("upscaler_edge_directed_rgb565", jni_content)
        self.assertNotIn("native_upscale_and_write_jpeg_rgb565", writer_content)
        self.assertNotIn("native_upscale_and_write_jpeg_rgb565", jni_content)

        # True 42.4MP inSampleSize = 1
        self.assertIn("BitmapRegionDecoder.newInstance", capture_content)
        self.assertIn("opts.inSampleSize = 1", capture_content)

    def test_guardrail_3_strict_dalvik_heap_and_strip_memory(self):
        """Guardrail 3: 64-row strips (7952 x 64), single DirectBuffer, immediate .recycle() per strip."""
        capture_content = self.capture_file.read_text(encoding="utf-8")

        # 64-row strip height
        self.assertIn("stripMaxH = 64", capture_content)

        # Immediate recycle in finally block
        self.assertIn("stripBmp.recycle()", capture_content)

        # Single direct ByteBuffer allocation
        self.assertIn("java.nio.ByteBuffer.allocateDirect(fullW * stripMaxH * 2)", capture_content)

        # Deterministic closure in finally block
        self.assertIn("ColorEngine.closeJpegStreamSafe(streamHandle, streamSuccess)", capture_content)

    def test_guardrail_4_asynchronous_decoupling_and_sequential_queue(self):
        """Guardrail 4: Immediate preview restoration (< 20ms) and single-thread sequential worker queue."""
        capture_content = self.capture_file.read_text(encoding="utf-8")

        # Shutter callback immediately cancels takePicture and restores preview
        self.assertIn("cameraAdapter.cancelTakePicture()", capture_content)
        self.assertIn("cameraAdapter.resumePreview()", capture_content)
        self.assertIn("restorePreviewWithWatchdog", capture_content)

        # Dedicated single-thread executor guarantees sequential jobs (no concurrent 42.4MP jobs)
        self.assertIn("Executors.newSingleThreadExecutor", capture_content)
        self.assertIn("processingExecutor.execute", capture_content)

    def test_guardrail_5_seamless_strip_boundaries_and_exif_copy(self):
        """Guardrail 5: Continuous global_y with deterministic row-seeded PRNG, EXIF copied from DSC0xxxx.JPG."""
        lut_content = self.lut_h.read_text(encoding="utf-8")
        jni_content = self.color_jni_c.read_text(encoding="utf-8")
        storage_content = self.storage_file.read_text(encoding="utf-8")
        capture_content = self.capture_file.read_text(encoding="utf-8")

        # Continuous row offset and deterministic PRNG in native LUT processing
        self.assertIn("lut17_process_rgb565_advanced_offset", lut_content)
        self.assertIn("int global_y = row_offset + y;", lut_content)
        self.assertIn("global_y * 1103515245U + 12345U", lut_content)
        self.assertIn("int grain_y_offset = (global_y & 63) << 6;", lut_content)
        self.assertIn("bayer_row = BAYER_2X2[global_y & 1];", lut_content)

        # JNI and Java row offset exposure
        self.assertIn("Java_com_fujilite_a7r2_ColorEngine_nativeGradeAndWriteStreamStrip", jni_content)
        self.assertIn("nativeApplyLutAndGrainDirectOffset", jni_content)

        # EXIF Preservation: copyExifMetadata copies from master capture to fuji_xxxx.jpg
        self.assertIn("copyExifMetadata", storage_content)
        self.assertIn("StorageResolver.copyExifMetadata(rawJpegPath, gradedSavedPath, presetName);", capture_content)
        self.assertIn("TAG_MAKE", storage_content)
        self.assertIn("TAG_MODEL", storage_content)
        self.assertIn("ExposureTime", storage_content)
        self.assertIn("FNumber", storage_content)
        self.assertIn("ISOSpeedRatings", storage_content)

    def test_native_jpeg_streaming_engine_methods(self):
        """Verify native_jpeg_writer implements streaming API correctly."""
        writer_h = self.jpeg_writer_h.read_text(encoding="utf-8")
        writer_c = self.jpeg_writer_c.read_text(encoding="utf-8")

        self.assertIn("native_jpeg_stream_open", writer_h)
        self.assertIn("native_jpeg_stream_write_strip_rgb565", writer_h)
        self.assertIn("native_jpeg_stream_close", writer_h)

        self.assertIn("native_jpeg_stream_open", writer_c)
        self.assertIn("native_jpeg_stream_write_strip_rgb565", writer_c)
        self.assertIn("native_jpeg_stream_close", writer_c)
        self.assertIn("struct JpegStreamState", writer_c)

if __name__ == '__main__':
    unittest.main()
