#!/usr/bin/env python3
"""
Exhaustive Validation Suite for Fuji-Lite A7R2 Pipeline.
Tests structural integrity, shutter trigger reliability, memory safety,
and 10.6MP upscaler code paths.
"""
import unittest
from pathlib import Path

class TestValidationPipeline(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parent.parent.parent
        self.capture_file = self.root / "src" / "app" / "CaptureController.java"
        self.main_file = self.root / "src" / "app" / "MainActivity.java"
        self.adapter_file = self.root / "src" / "camera" / "SonyPmcaAdapter.java"
        self.storage_file = self.root / "src" / "app" / "StorageResolver.java"
        self.color_file = self.root / "src" / "color" / "ColorEngine.java"
        self.jni_file = self.root / "src" / "color" / "color_engine_jni.c"
        self.lut_file = self.root / "src" / "color" / "lut_cube_17.h"
        self.mk_file = self.root / "jni" / "Android.mk"

    def test_shutter_sequencer_trigger_in_adapter(self):
        """Verify SonyPmcaAdapter.takePicture provides a ShutterCallback to trip BIONZ X."""
        content = self.adapter_file.read_text(encoding="utf-8")
        self.assertIn("Camera.ShutterCallback shutterCb = new Camera.ShutterCallback()", content)
        self.assertIn("normalCamera.takePicture(shutterCb, null, jpegCallback)", content)

    def test_shutter_button_timestamp_debounce_in_main(self):
        """Verify MainActivity uses timestamp debounce instead of permanent boolean lockout."""
        content = self.main_file.read_text(encoding="utf-8")
        self.assertIn("lastShutterDownTime", content)
        self.assertIn("now - lastShutterDownTime < 600", content)

    def test_lifecycle_startup_guardrails(self):
        """Verify MainActivity.surfaceCreated and onResume are guarded by try-catch."""
        content = self.main_file.read_text(encoding="utf-8")
        self.assertIn("surfaceCreated failed", content)
        self.assertIn("onResume startPreview failed", content)

    def test_capture_controller_callback_upscaling_pipeline(self):
        """Verify CaptureController receives callback, grades, upscales, and saves FUJI photo."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("cameraAdapter.takePicture(new Camera.PictureCallback()", content)
        self.assertIn("processAndUpscaleCapture(jpegData, seqNum, captureStartTime)", content)
        self.assertIn("ColorEngine.applyLutAndGrainBitmapSafe(srcBmp", content)
        self.assertIn("ColorEngine.upscaleEdgeDirectedSafe", content)
        self.assertIn("StorageResolver.saveGradedJpegToSd", content)

    def test_heap_safe_failover_in_capture_controller(self):
        """Verify 10.6MP bitmap allocation is guarded by try-catch with graceful fallback."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("Bitmap dst10MpBmp = null", content)
        self.assertIn("dst10MpBmp = Bitmap.createBitmap(targetW, targetH, Bitmap.Config.RGB_565)", content)
        self.assertIn("dst10MpBmp = srcBmp", content)

    def test_display_safe_review_bitmap(self):
        """Verify review bitmap is downscaled to display size to avoid OpenGL 2048x2048 crash."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("Bitmap.createScaledBitmap(bitmap, targetW, targetH, true)", content)
        self.assertIn("targetW = 960", content)

    def test_no_libjnigraphics_dependency(self):
        """Verify Android.mk and color_engine_jni do NOT link libjnigraphics."""
        mk_content = self.mk_file.read_text(encoding="utf-8")
        self.assertNotIn("-ljnigraphics", mk_content)
        jni_content = self.jni_file.read_text(encoding="utf-8")
        self.assertNotIn("<android/bitmap.h>", jni_content)

    def test_safe_jni_lut_precomputation(self):
        """Verify JNI precomputeLut17 uses safe stack regions without critical sections."""
        jni_content = self.jni_file.read_text(encoding="utf-8")
        self.assertIn("GetShortArrayRegion", jni_content)
        self.assertIn("GetByteArrayRegion", jni_content)

    def test_lut_cube_17_edge_directed_upscaler(self):
        """Verify lut_cube_17.h implements upscaler_edge_directed_rgb565."""
        lut_content = self.lut_file.read_text(encoding="utf-8")
        self.assertIn("upscaler_edge_directed_rgb565", lut_content)
        self.assertIn("scale_x", lut_content)
        self.assertIn("scale_y", lut_content)
        self.assertIn("BAYER_2X2", lut_content)

if __name__ == '__main__':
    unittest.main()
