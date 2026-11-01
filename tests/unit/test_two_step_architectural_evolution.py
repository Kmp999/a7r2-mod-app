#!/usr/bin/env python3
"""
Unit Test Suite for Two-Step Architectural Evolution:
  Step 1: Optional Full-Resolution 42.4MP "Master Grade" Mode vs. Fast 10.6MP Mode
          - OutputResolutionMode enum in AppState (FAST_10MP vs MASTER_42MP)
          - Q-Menu toggle on Item 15 (OUT RES)
          - Strict 16x16 MCU strip alignment (128 src rows -> 64 out rows for 10.6MP; 64 src rows -> 64 out rows for 42.4MP)
          - Peak direct buffer memory capped at 2.03 MB (7952 * 64 * 4)
          - Contiguous outputRowOffset incrementing by 64 across strips
          - Dynamic EXIF dimensions (3976x2652 vs 7952x5304) and telemetry toast (10.6MP vs 42.4MP)
  Step 2: RAW+JPEG Hardware Guardrails & DR200/DR400 Compensatory Tone-Curve Lift
          - CameraEx and Camera.Parameters format guardrails (enforces JPEG / RAW+JPEG, fine quality)
          - Hardened waitForJpegComplete in StorageResolver (ignores .ARW, scales timeout >= 5000ms)
          - Non-linear shadow/midtone lift (+1.0 EV for DR200, +2.0 EV for DR400 with highlight roll-off) in FujiRecipe
          - EXIF UserComment stamps active DR mode ("| DR: <DRMode>")
"""

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

class TestTwoStepArchitecturalEvolution(unittest.TestCase):
    def setUp(self):
        self.src_dir = REPO_ROOT / "src"
        self.app_state_file = self.src_dir / "app" / "AppState.java"
        self.qmenu_file = self.src_dir / "app" / "FujiQMenuView.java"
        self.capture_file = self.src_dir / "app" / "CaptureController.java"
        self.storage_file = self.src_dir / "app" / "StorageResolver.java"
        self.mapper_file = self.src_dir / "camera" / "SonyToFujiHardwareMapper.java"
        self.recipe_file = self.src_dir / "app" / "FujiRecipe.java"

    # ------------------------------------------------------------------------
    # STEP 1: OutputResolutionMode & Strip Memory Guardrails
    # ------------------------------------------------------------------------
    def test_app_state_output_resolution_mode(self):
        """Step 1A: AppState defines OutputResolutionMode with FAST_10MP and MASTER_42MP."""
        content = self.app_state_file.read_text(encoding="utf-8")
        self.assertIn("enum OutputResolutionMode", content)
        self.assertIn("FAST_10MP", content)
        self.assertIn("MASTER_42MP", content)
        self.assertIn("3976, 2652, 2", content)
        self.assertIn("7952, 5304, 1", content)
        self.assertIn("toggleOutputResolutionMode", content)

    def test_qmenu_resolution_toggle_item(self):
        """Step 1B: FujiQMenuView wires Item 15 to OUT RES and toggles resolution mode."""
        content = self.qmenu_file.read_text(encoding="utf-8")
        self.assertIn('itemTitles[15] = "OUT RES"', content)
        self.assertIn("appState.getOutputResolutionMode().label", content)
        self.assertIn("appState.toggleOutputResolutionMode()", content)

    def test_capture_controller_dual_resolution_strip_logic(self):
        """Step 1C: CaptureController supports both FAST_10MP and MASTER_42MP strip geometry."""
        content = self.capture_file.read_text(encoding="utf-8")
        self.assertIn("resMode == AppState.OutputResolutionMode.MASTER_42MP", content)
        self.assertIn("targetSample = 1", content)
        self.assertIn("srcStripH = 64", content)
        self.assertIn("srcStripH = 128", content)
        # Sized to 2.03 MB max cap (7952 * 64 * 4)
        self.assertIn("java.nio.ByteBuffer.allocateDirect(dstW * 64 * 4)", content)
        # Contiguous offset increment
        self.assertIn("outputRowOffset += outStripH", content)

    # ------------------------------------------------------------------------
    # STEP 2: RAW+JPEG Guardrails & Compensatory DR Tone Curve
    # ------------------------------------------------------------------------
    def test_sony_mapper_enforces_raw_jpeg_guardrails(self):
        """Step 2A: SonyToFujiHardwareMapper enforces JPEG or RAW+JPEG format so DSC0xxxx.JPG is guaranteed."""
        content = self.mapper_file.read_text(encoding="utf-8")
        self.assertIn('"setPictureStorageFormat"', content)
        self.assertIn('"raw+jpeg"', content)
        self.assertIn('"setImageQuality"', content)
        self.assertIn('"fine"', content)
        self.assertIn('params.set("sony-quality", "fine")', content)
        self.assertIn('params.set("sony-picture-format", "raw+jpeg")', content)

    def test_storage_resolver_hardened_file_wait(self):
        """Step 2B: StorageResolver.waitForJpegComplete ignores .ARW and scales timeout >= 5000ms."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn('endsWith(".JPG")', content)
        self.assertIn("Math.max(timeoutMs, 5000)", content)
        self.assertIn("Thread.sleep(40)", content)

    def test_fuji_recipe_compensatory_dr_tone_curve_lift(self):
        """Step 2C: FujiRecipe applies non-linear shadow/midtone lift (+1.0EV DR200, +2.0EV DR400)."""
        content = self.recipe_file.read_text(encoding="utf-8")
        self.assertIn("dynamicRange == DynamicRange.DR200", content)
        self.assertIn("0.18 * (1.0 - Math.pow(x, 1.5))", content)
        self.assertIn("dynamicRange == DynamicRange.DR400", content)
        self.assertIn("0.32 * (1.0 - Math.pow(x, 1.3))", content)

    def test_exif_cloning_stamps_dr_mode(self):
        """Step 2D: StorageResolver stamps active DR mode into EXIF UserComment."""
        content = self.storage_file.read_text(encoding="utf-8")
        self.assertIn('" | DR: "', content)
        self.assertIn('dstExif.setAttribute("UserComment", userComment)', content)

if __name__ == '__main__':
    unittest.main()
