# Fuji-Inspired Color Science Approximations

## Legal & Scientific Disclaimer
The presets in this repository are **original mathematical approximations** inspired by general characteristics of reversal/negative films and classical color palettes. They are **not** official Fujifilm Film Simulations and do **not** claim to reproduce or copy proprietary Fujifilm recipes, calibration curves, or copyrighted lookup tables.

---

## Color Processing Pipeline Architecture
The color transformation engine executes in integer/fixed-point arithmetic using standard matrix-LUT decomposition:

$$\begin{bmatrix} R' \\ G' \\ B' \end{bmatrix} = \text{LUT}_{1D} \left( M_{3\times3} \cdot \begin{bmatrix} R \\ G \\ B \end{bmatrix} \right)$$

Where:
1. $M_{3\times3}$ is a $3\times3$ Color Correction Matrix (CCM) represented in Q8.8 fixed-point format ($1.0 = 256$).
2. $\text{LUT}_{1D}$ consists of three independent 256-byte lookup tables (one each for Red, Green, and Blue channels) encoding non-linear tone response (S-curve, shadow lift, highlight rolloff).
3. Optional Chroma scaling adjusts saturation and split toning.

---

## Preset Formulations

### 1. Provia-like (Standard Reversal)
*   **Aesthetic:** Neutral, natural saturation, balanced daylight contrast.
*   **3x3 Matrix (Q8.8):**
    $$\begin{bmatrix} 266 & -5 & -5 \\ -5 & 266 & -5 \\ -5 & -5 & 266 \end{bmatrix} / 256$$
*   **Tone Curve:** Mild S-curve with linear midtones.
    - Shadow: Slight toe at 16.
    - Highlight: Rolloff above 235.
*   **Target Cost:** < 45 ms at 720p.

### 2. Astia-like (Soft Portrait)
*   **Aesthetic:** Soft skin tones, reduced red contrast, gentle pastel highlights.
*   **3x3 Matrix (Q8.8):**
    $$\begin{bmatrix} 248 & 6 & 2 \\ 2 & 254 & 0 \\ 0 & 4 & 252 \end{bmatrix} / 256$$
*   **Tone Curve:** Flatter contrast in skin-tone luminance range (RGB 120-190).
*   **Target Cost:** < 45 ms at 720p.

### 3. Velvia-like (Vivid Slide)
*   **Aesthetic:** Deep punchy blacks, saturated blues and foliage greens.
*   **3x3 Matrix (Q8.8):**
    $$\begin{bmatrix} 290 & -20 & -14 \\ -16 & 295 & -23 \\ -10 & -25 & 291 \end{bmatrix} / 256$$
*   **Tone Curve:** Steep S-curve, crushed dark shadows (toe crushed below 24).
*   **Target Cost:** < 48 ms at 720p.

### 4. Classic Neg-like (Vintage Negative)
*   **Aesthetic:** Lifted cyan/green shadows, warm highlight bias, muted reds, high midtone contrast.
*   **3x3 Matrix (Q8.8):**
    $$\begin{bmatrix} 230 & 16 & 10 \\ -12 & 270 & -2 \\ 10 & -15 & 261 \end{bmatrix} / 256$$
*   **Tone Curve:** Hard midtone transition, lifted black level (minimum output ~ 12).
*   **Target Cost:** < 50 ms at 720p.

### 5. Eterna-like (Cinema Soft)
*   **Aesthetic:** Low saturation, extremely soft contrast, lifted shadows, wide dynamic range look.
*   **3x3 Matrix (Q8.8):**
    $$\begin{bmatrix} 210 & 25 & 21 \\ 20 & 215 & 21 \\ 20 & 24 & 212 \end{bmatrix} / 256$$
*   **Tone Curve:** Soft contrast, highlight rolloff begins at 180, lifted blacks (minimum 16).
*   **Target Cost:** < 42 ms at 720p.
