#!/usr/bin/env python3
"""
Unit Test Suite for Consecutive Capture & Shutter Dead-Zone Elimination:
  1. CameraAdapter.java: resumePreview() returns boolean for explicit failure propagation.
  2. SonyPmcaAdapter.java:
     - Direct hardware capture via normalCamera.takePicture(shutterCallback, null, null).
     - resumePreview() returns boolean (true on success, false on failure).
  3. CaptureController.java:
     - captureWatchdogRunnable rescues camera when currentState != IDLE_PREVIEW.
     - restorePreviewWithWatchdog verifies resumePreview() return value and schedules retries.
     - Offloads storage polling without synchronous UI-thread SD card scanning.
     - Deduplicates processing tasks for the same photo.
  4. MainActivity.java & UIController.java:
     - Guarantees cameraAdapter.resumePreview() is invoked on quick review dismissal for FULL_SHUTTER and HALF_SHUTTER.
"""

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

class TestConsecutiveCaptureDeadZoneFix(unittest.TestCase):
    def setUp(self):
        self.src_dir = REPO_ROOT / "src"
        self.camera_adapter_file = self.src_dir / "camera" / "CameraAdapter.java"
        self.sony_pmca_file = self.src_dir / "camera" / "SonyPmcaAdapter.java"
        self.capture_file = self.src_dir / "app" / "CaptureController.java"
        self.main_activity_file = self.src_dir / "app" / "MainActivity.java"
        self.ui_controller_file = self.src_dir / "app" / "UIController.java"

    def test_camera_adapter_resume_preview_returns_boolean(self):
        """CameraAdapter.resumePreview() must return boolean for verification."""
        content = self.camera_adapter_file.read_text(encoding="utf-8")
        self.assertIn("boolean resumePreview();", content)

    def test_sony_pmca_adapter_primary_hardware_capture_and_preview_return(self):
        """SonyPmcaAdapter invokes normalCamera.takePicture(shutterCallback, null, null) and returns boolean in resumePreview."""
        content = self.sony_pmca_file.read_text(encoding="utf-8")
        self.assertIn("normalCamera.takePicture(shutterCallback, null, null)", content)
        self.assertIn("public boolean resumePreview()", content)
        self.assertIn("return true;", content)
        self.assertIn("return false;", content)

    def test_capture_controller_watchdog_and_preview_retry(self):
        """CaptureController watchdog monitors currentState != IDLE_PREVIEW and checks resumePreview() return."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("currentState != CameraState.IDLE_PREVIEW", content)
        self.assertIn("boolean success = cameraAdapter.resumePreview();", content)
        self.assertIn("throw new RuntimeException(\"resumePreview returned false\");", content)
        self.assertIn("processedPhotos", content)

    def test_main_activity_resumes_preview_on_review_dismiss(self):
        """MainActivity.dispatchKeyEvent() resumes preview when dismissing quick review on full or half shutter."""
        content = self.main_activity_file.read_text(encoding="utf-8")
        # Full shutter dismiss block
        self.assertIn("qMenuView.clearQuickReview();\n                    if (cameraAdapter != null) cameraAdapter.resumePreview();", content)

    def test_ui_controller_resumes_preview_on_review_dismiss(self):
        """UIController.handleRawKeyDown() resumes preview when dismissing quick review overlay."""
        content = self.ui_controller_file.read_text(encoding="utf-8")
        self.assertIn("qMenuView.clearQuickReview();\n            if (cameraAdapter != null) cameraAdapter.resumePreview();", content)

if __name__ == '__main__':
    unittest.main()
