#!/usr/bin/env python3
"""
Unit tests verifying keycode mappings, Playback resolution, and DCF file generation rules.
"""
import re
import unittest
from pathlib import Path

class TestKeycodesAndPlayback(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[2]
        self.keycodes_file = self.root / "src" / "camera" / "SonyKeyCodes.java"
        self.storage_file = self.root / "src" / "app" / "StorageResolver.java"

    def test_play_keycode_mapping_not_shutter(self):
        """Verify that 516 and 0x204 are mapped to PLAY, not FULL_SHUTTER."""
        content = self.keycodes_file.read_text(encoding="utf-8")
        
        # Check that 516 maps to PLAY
        m516 = re.search(r"sKeyCodeMap\.put\(\s*516\s*,\s*FujiAction\.([A-Z_]+)\s*\)", content)
        self.assertIsNotNone(m516, "516 mapping must exist")
        self.assertEqual(m516.group(1), "PLAY", "Key code 516 must map to PLAY")

        # Check that 0x204 maps to PLAY
        m204 = re.search(r"sKeyCodeMap\.put\(\s*0x204\s*,\s*FujiAction\.([A-Z_]+)\s*\)", content)
        self.assertIsNotNone(m204, "0x204 mapping must exist")
        self.assertEqual(m204.group(1), "PLAY", "Key code 0x204 must map to PLAY")

        # Check that 514 maps to FULL_SHUTTER
        m514 = re.search(r"sKeyCodeMap\.put\(\s*514\s*,\s*FujiAction\.([A-Z_]+)\s*\)", content)
        self.assertIsNotNone(m514, "514 mapping must exist")
        self.assertEqual(m514.group(1), "FULL_SHUTTER", "Key code 514 must map to FULL_SHUTTER")

    def test_exif_metadata_written_on_save(self):
        """Verify that StorageResolver writes EXIF tags for DCF compliance."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("ExifInterface", content)
        self.assertIn("TAG_MAKE", content)
        self.assertIn("TAG_MODEL", content)
        self.assertIn("writeExifMetadata", content)

    def test_avindex_provider_registration(self):
        """Verify that AvindexStore ContentProvider insert is handled."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("EXTERNAL_CONTENT_URI", content)
        self.assertIn("getContentResolver().insert", content)

    def test_playback_helper_methods_exist(self):
        """Verify that StorageResolver provides photo loading helpers for playback."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn("getLatestSavedPhoto", content)
        self.assertIn("loadPhotoForReview", content)

if __name__ == '__main__':
    unittest.main()
