#!/usr/bin/env python3
"""
LUT Generator for Fuji-Lite-A7R2.
Precomputes S-curves and tonal responses to avoid any runtime math on the camera CPU.
"""

import math
import sys

def generate_scurve(shadow_lift=0, highlight_rolloff=0, contrast_boost=0):
    lut = []
    contrast_factor = 1.0 + (contrast_boost / 100.0)
    for i in range(256):
        x = i / 255.0
        centered = (x - 0.5) * contrast_factor
        s = 1.0 / (1.0 + math.exp(-6.0 * centered))
        
        if x < 0.3:
            s += (shadow_lift / 255.0) * ((0.3 - x) / 0.3)
        if x > 0.7:
            s -= (highlight_rolloff / 255.0) * ((x - 0.7) / 0.3)
            
        val = int(round(max(0.0, min(1.0, s)) * 255.0))
        lut.append(val)
    return lut

def main():
    presets = {
        "provia": generate_scurve(0, 0, 15),
        "astia": generate_scurve(5, -10, 8),
        "velvia": generate_scurve(-15, 10, 40),
        "classic_neg": generate_scurve(15, 20, 25),
        "eterna": generate_scurve(20, 30, -10),
    }

    print("/* Auto-generated Fuji-Lite 1D LUT Tables */")
    for name, lut in presets.items():
        print(f"static const uint8_t LUT_{name.upper()}[256] = {{")
        for chunk in [lut[i:i+16] for i in range(0, 256, 16)]:
            print("    " + ", ".join(f"0x{v:02X}" for v in chunk) + ",")
        print("};\n")

if __name__ == "__main__":
    main()
