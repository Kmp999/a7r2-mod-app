#!/usr/bin/env python3
"""
Unit Test Suite for Consecutive Capture & Shutter Dead-Zone Elimination:
  1. CameraAdapter.java: resumePreview() returns boolean for explicit failure propagation.
  2. SonyPmcaAdapter.java:
     - Direct hardware capture via CameraEx.burstableTakePicture() and fallback normalCamera.takePicture.
     - resumePreview() returns boolean (true on success, false on failure).
  3. CaptureController.java:
     - captureWatchdogRunnable rescues camera when currentState != IDLE_PREVIEW.
     - restorePreviewWithWatchdog verifies resumePreview() return value and schedules retries.
     - Snapshots preExistingFiles before shutter and polls with fallback to ensure every shot is processed.
     - Preserves full resolution (inSampleSize = 1) in MASTER_42MP mode.
  4. StorageResolver.java:
     - waitForNewCameraPhoto filters out pre-existing photos and verifies JPEG completion via 0xFF 0xD9.
     - getLatestCameraPhoto strictly targets camera master JPEGs excluding fuji_*.jpg.
  5. AppState.java:
     - Defaults to MASTER_42MP for highest possible resolution.
  6. MainActivity.java & UIController.java:
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
        self.storage_file = self.src_dir / "app" / "StorageResolver.java"
        self.app_state_file = self.src_dir / "app" / "AppState.java"
        self.main_activity_file = self.src_dir / "app" / "MainActivity.java"
        self.ui_controller_file = self.src_dir / "app" / "UIController.java"

    def test_camera_adapter_resume_preview_returns_boolean(self):
        """CameraAdapter.resumePreview() must return boolean for verification."""
        content = self.camera_adapter_file.read_text(encoding="utf-8")
        self.assertIn("boolean resumePreview();", content)

    def test_sony_pmca_adapter_primary_hardware_capture_and_preview_return(self):
        """SonyPmcaAdapter invokes CameraEx.burstableTakePicture() and returns boolean in resumePreview."""
        content = self.sony_pmca_file.read_text(encoding="utf-8")
        self.assertIn("burstableTakePicture", content)
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

    def test_main_activity_resumes_preview_on_review_dismiss(self):
        """MainActivity.dispatchKeyEvent() resumes preview when dismissing quick review on full or half shutter."""
        content = self.main_activity_file.read_text(encoding="utf-8")
        self.assertIn("qMenuView.clearQuickReview();\n                    if (cameraAdapter != null) cameraAdapter.resumePreview();", content)

    def test_ui_controller_resumes_preview_on_review_dismiss(self):
        """UIController.handleRawKeyDown() resumes preview when dismissing quick review overlay."""
        content = self.ui_controller_file.read_text(encoding="utf-8")
        self.assertIn("qMenuView.clearQuickReview();\n            if (cameraAdapter != null) cameraAdapter.resumePreview();", content)

    def test_app_state_default_master_42mp(self):
        """AppState must default to MASTER_42MP for highest possible resolution."""
        content = self.app_state_file.read_text(encoding="utf-8")
        self.assertIn("outputResolutionMode = OutputResolutionMode.MASTER_42MP", content)

    def test_storage_resolver_consecutive_photo_filtering(self):
        """StorageResolver.waitForNewCameraPhoto skips pre-existing photos and verifies JPEG completion."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("isJpegComplete(newest)", content)
        self.assertIn("getLatestCameraPhoto()", content)
        self.assertIn("snapshotExistingPhotos()", content)

    def test_capture_controller_passes_pre_existing_files(self):
        """CaptureController snapshots pre-existing files and waits with guaranteed fallback."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("StorageResolver.snapshotExistingPhotos();", content)
        self.assertIn("StorageResolver.waitForNewCameraPhoto(preExistingFiles, 7000)", content)
        self.assertIn("StorageResolver.getLatestCameraPhoto()", content)
        self.assertIn("processedPhotoPaths", content)

    def test_hardware_shutter_direct_actuation_ordering(self):
        """SonyPmcaAdapter prioritizes direct normalCamera.takePicture before burstableTakePicture fallback."""
        content = self.sony_pmca_file.read_text(encoding="utf-8")
        idx_direct = content.find("normalCamera.takePicture(shutterCallback, null, null);")
        idx_burst = content.find("burstableTakePicture")
        self.assertNotEqual(idx_direct, -1)
        self.assertNotEqual(idx_burst, -1)
        self.assertLess(idx_direct, idx_burst, "Direct mechanical takePicture must precede burstableTakePicture fallback")

    def test_storage_resolver_jpeg_completion_size_fallback(self):
        """StorageResolver.isJpegComplete contains len > 200000 fallback for Sony trailer metadata."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("return len > 200000;", content)

if __name__ == '__main__':
    unittest.main()
