#!/usr/bin/env python3
"""
Benchmark Log Parser for Fuji-Lite-A7R2.
Parses timing profiler stdout and outputs a standardized Markdown table for BENCHMARK.md.
"""

import sys
import re

def parse_log(text):
    stage_pattern = re.compile(r"^\s*([a-zA-Z0-9_]+)\s*:\s*([0-9\.]+)\s*ms", re.MULTILINE)
    matches = stage_pattern.findall(text)
    
    if not matches:
        print("No profiler stages found in log input.")
        return

    print("| Stage | Duration (ms) |")
    print("| :--- | :--- |")
    total_ms = 0.0
    for stage, ms_str in matches:
        ms = float(ms_str)
        print(f"| `{stage}` | {ms:.2f} ms |")
        total_ms += ms

    print(f"\n**Total Measured App Duration:** {total_ms:.2f} ms")
    if total_ms <= 500.0:
        print("**Verdict:** PASS (Under 500 ms target)")
    else:
        print("**Verdict:** FAIL (Exceeded 500 ms target)")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'r') as f:
            parse_log(f.read())
    else:
        # Sample run
        sample = """
        app_read_image : 12.40 ms
        app_color_matrix : 54.10 ms
        app_tone_curve_lut : 31.80 ms
        app_encode_output : 110.20 ms
        app_save_output : 45.30 ms
        app_ui_update : 4.10 ms
        """
        parse_log(sample)
