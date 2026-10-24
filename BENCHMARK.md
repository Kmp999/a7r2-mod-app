# Benchmarking Plan

## Overview
To ensure the app meets the 500 ms target on the Sony A7R II, we must systematically benchmark the JNI color engine and the Android Camera API overhead.

## Procedure
1.  **Warm-up:** Discard the first 3 captures/frames to allow the Dalvik VM and JNI interface to settle.
2.  **Number of shots:** Run 10 continuous captures/frames per variant.
3.  **Conditions:**
    *   Battery > 50% (prevents underclocking).
    *   Camera body temperature < 40°C.
    *   SD Card: UHS-I U3 or faster.
    *   UI rendering: Disabled during benchmark.

## Variants to Test
1.  `640x480` NV21 direct processing.
2.  `1280x720` NV21 direct processing.
3.  `1920x1080` NV21 direct processing.
4.  `1920x1080` JPEG proxy -> decode -> process -> encode.

## Result Template
```markdown
| Resolution | Format | Decode (ms) | Filter Matrix (ms) | Filter LUT (ms) | Encode (ms) | Save (ms) | Total (ms) | Pass/Fail |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| 1280x720 | NV21 | N/A | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| 1920x1080| NV21 | N/A | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| 1920x1080| JPEG | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
```

## Pass/Fail Criteria
*   **PASS:** Total time from image available to save completion < 500 ms.
*   **FAIL:** Total time > 500 ms, causing UI thread freezing or dropped frames in preview mode.
