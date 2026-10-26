#!/usr/bin/env python3
"""
Unit tests verifying the expanded film simulation presets, 3D LUT assets, and recipe alignment.
"""
import os
import re
import unittest
from pathlib import Path

class TestFilmSimulations(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[2]
        self.assets_presets = self.root / "assets" / "presets"
        self.recipe_file = self.root / "src" / "app" / "FujiRecipe.java"
        self.appstate_file = self.root / "src" / "app" / "AppState.java"

    def test_bundled_luts_exist_and_sized_correctly(self):
        """Verify that all bundled .lut files in assets/presets are exactly 14,739 bytes."""
        lut_files = list(self.assets_presets.glob("*.lut"))
        self.assertGreaterEqual(len(lut_files), 25, "Must have at least 25 bundled .lut files")
        expected_size = 17 * 17 * 17 * 3  # 14,739 bytes
        for lf in lut_files:
            size = lf.stat().st_size
            self.assertEqual(size, expected_size, f"{lf.name} must be exactly {expected_size} bytes, got {size}")

    def test_recipe_film_sim_enum_contains_key_stocks(self):
        """Verify that FujiRecipe.FilmSim includes Fujifilm, Kodak, and Cinema stocks."""
        content = self.recipe_file.read_text(encoding="utf-8")
        required_sims = [
            "PROVIA", "VELVIA", "ASTIA", "CLASSIC_CHROME", "CLASSIC_NEG",
            "PRO_NEG_HI", "PRO_NEG_STD", "ETERNA", "ACROS",
            "FUJI_REALA_500D", "FUJI_ETERNA_250D", "FUJI_F125", "FUJI_3510", "FUJI_3513DI",
            "KODAK_PORTRA_5205", "KODAK_VISION_5218", "KODAK_2383_D60",
            "KODAK_FILMSTOCK_50", "AEROLUX", "TEAL_ORANGE", "TENSION_GREEN"
        ]
        for sim in required_sims:
            self.assertIn(sim, content, f"FilmSim enum must contain {sim}")

    def test_appstate_preset_names_match_filmsim(self):
        """Verify AppState.PRESET_NAMES has expanded presets."""
        content = self.appstate_file.read_text(encoding="utf-8")
        self.assertIn("Portra 5205", content)
        self.assertIn("Kodak 2383", content)
        self.assertIn("Reala 500D", content)
        self.assertIn("Teal & Orange", content)

if __name__ == '__main__':
    unittest.main()
