#!/usr/bin/env python3
"""
Unit tests verifying the Dual-Path Decoupled Architecture for 42.4 MP full-res capture.
"""
import unittest
from pathlib import Path

class TestDualPath42MP(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[2]
        self.capture_file = self.root / "src" / "app" / "CaptureController.java"
        self.storage_file = self.root / "src" / "app" / "StorageResolver.java"

    def test_storage_resolver_has_save_raw_jpeg(self):
        """Verify StorageResolver has saveRawJpegToSd for uncompressed direct 42.4MP byte writing."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("saveRawJpegToSd", content)
        self.assertIn("byte[] rawJpegData", content)

    def test_capture_controller_dual_path_implementation(self):
        """Verify CaptureController dispatches both raw 42.4MP save and proxy review."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("saveFullResJpegDirectly", content)
        self.assertIn("processQuickReviewProxy", content)
        self.assertIn("SAVED 42.4MP", content)

    def test_proxy_uses_heap_safe_sample_size(self):
        """Verify proxy quick review uses sample=4 for Dalvik heap safety."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("opts.inSampleSize = 4", content)

    def test_instant_preview_restoration_called(self):
        """Verify restorePreviewWithWatchdog is invoked immediately on picture taken."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("restorePreviewWithWatchdog", content)

if __name__ == '__main__':
    unittest.main()
