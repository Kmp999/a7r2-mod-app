#ifndef NATIVE_JPEG_FAST_STREAM_H
#define NATIVE_JPEG_FAST_STREAM_H

#include "lut_cube_17.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Extended telemetry and zero-I/O in-memory Quick Review preview returned by
 * native_fast_transcode_jpeg_ex().
 */
typedef struct {
    int rc;                   // 1 on success, <=0 error stage code
    int total_ms;             // Total wall time in ms
    int dec_ms;               // Blocking time spent in Stage 1 (SD read + Huffman decode)
    int proc_ms;              // Time spent in Stage 2 (parallel IDCT + YCbCr LUT + FDCT + Quant)
    int emit_ms;              // Blocking time spent waiting on Stage 3 (Huffman emit + SD write)
    int ncpu;                 // Detected online CPU cores from /proc/cpuinfo
    int has_neon;             // 1 if /proc/cpuinfo reports neon/asimd
    int is_v7;                // 1 if compiled for ARMv7-A (armeabi-v7a)
    uint16_t* preview_rgb565; // Allocated RGB_565 DC preview (caller frees via free()), or NULL
    int preview_w;
    int preview_h;
} FastTranscodeResult;

/**
 * Pure-C Single-Pass 3-Stage Overlapped Multi-Core Baseline JPEG-to-JPEG Transcoder.
 *
 * Architecture:
 *  - Pre-bakes a 19.6KB L1-cache-resident 17^3 direct YCbCr -> YCbCr 3D LUT once per image
 *    with exact same-cell 2x2 quad fast path and precomputed 256-byte luma grain table.
 *  - Dedicated half-height vertical AAN IDCT (idct_8x4_half_v_aan) for Sony 4:2:2 chroma.
 *  - 3-stage overlapped pipeline: double-buffered in_band overlaps SD read + Huffman decode
 *    of Band N+1 with multi-core IDCT/LUT/FDCT of Band N and async Huffman emit of Band N-1.
 *  - Zero-I/O RGB565 Quick Review preview extracted directly from FDCT DC coefficients
 *    (eliminates post-write SD re-read & Skia re-decode of the full 17.7MP output JPEG).
 *
 * Returns:
 *   1  on complete success
 *  <=0 on unsupported format or I/O error (-9 = output open failed -> retry Tier 2/3).
 */
int native_fast_transcode_jpeg_ex(const Lut17Context* lut,
                                  const char* input_jpeg_path,
                                  const char* output_jpeg_path,
                                  int scale_denom,
                                  int quality,
                                  int grain_strength,
                                  int clarity_strength,
                                  FastTranscodeResult* out_result);

int native_fast_transcode_jpeg(const Lut17Context* lut,
                               const char* input_jpeg_path,
                               const char* output_jpeg_path,
                               int scale_denom,
                               int quality,
                               int grain_strength,
                               int clarity_strength);

#ifdef __cplusplus
}
#endif

#endif // NATIVE_JPEG_FAST_STREAM_H
