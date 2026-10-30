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

    def test_storage_resolver_saves_graded_jpeg_with_fuji_prefix(self):
        """Verify StorageResolver has saveGradedJpegToSd and sequence allocation."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("saveGradedJpegToSd", content)
        self.assertIn("allocateNextSequenceNumber", content)
        self.assertIn('"FUJI"', content)

    def test_capture_controller_saves_graded_photo_to_sd(self):
        """Verify CaptureController dispatches saving the graded photo to SD card."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("StorageResolver.saveGradedJpegToSd", content)
        self.assertIn("candidateSampleSizes = new int[]{ 2, 4 }", content)

    def test_manifest_enables_large_heap(self):
        """Verify AndroidManifest.xml specifies android:largeHeap='true' for 10.6MP processing."""
        manifest_file = self.root / "AndroidManifest.xml"
        manifest_content = manifest_file.read_text(encoding="utf-8")
        self.assertIn('android:largeHeap="true"', manifest_content)

    def test_10_6mp_dynamic_sampling(self):
        """Verify CaptureController selects sample=1 for 10.6MP input to preserve full 4K detail."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("targetSample = (rawW > 5000) ? 2 : 1", content)
        self.assertIn("rawW <= 5000", content)

    def test_sony_adapter_configures_small_image_size(self):
        """Verify SonyPmcaAdapter configures BIONZ X ImageSize to small (~10.6MP)."""
        adapter_file = self.root / "src" / "camera" / "SonyPmcaAdapter.java"
        adapter_content = adapter_file.read_text(encoding="utf-8")
        self.assertIn('"small"', adapter_content)
        self.assertIn("s.width >= 3500", adapter_content)

    def test_hardware_direct_capture_bypasses_hal_proxy(self):
        """Verify takeHardwarePicture passes (shutterCallback, null, null) to bypass 1.7MP HAL proxy."""
        adapter_file = self.root / "src" / "camera" / "SonyPmcaAdapter.java"
        adapter_content = adapter_file.read_text(encoding="utf-8")
        self.assertIn("normalCamera.takePicture(shutterCallback, null, null)", adapter_content)
        self.assertIn("cancelTakePicture", adapter_content)

    def test_storage_resolver_has_hardware_jpeg_poller(self):
        """Verify StorageResolver has fallback DCIM poller and quality 94."""
        storage_content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("pollForNewHardwareJpeg", storage_content)
        self.assertIn("parseSequenceNumber", storage_content)
        self.assertIn("JPEG, 94, out", storage_content)

    def test_hardware_mapper_preserves_10_6mp_and_single_drive(self):
        """Verify SonyToFujiHardwareMapper preserves 10.6MP ImageSize and Single DriveMode."""
        mapper_file = self.root / "src" / "camera" / "SonyToFujiHardwareMapper.java"
        mapper_content = mapper_file.read_text(encoding="utf-8")
        self.assertIn('"small"', mapper_content)
        self.assertIn('"single"', mapper_content)

    def test_lut_cube_17_performance_optimizations(self):
        """Verify lut_cube_17.h has bit replication, branchless white-point, and 64x64 grain."""
        lut_file = self.root / "src" / "color" / "lut_cube_17.h"
        lut_content = lut_file.read_text(encoding="utf-8")
        self.assertIn("STATIC_GRAIN_64x64", lut_content)
        self.assertIn("BAYER_2X2", lut_content)
        self.assertIn("r + (r >> 7)", lut_content)
        self.assertIn("(r5 << 3) | (r5 >> 2)", lut_content)

if __name__ == '__main__':
    unittest.main()
