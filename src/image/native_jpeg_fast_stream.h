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
    // ---- Diagnostics (answer "slow silicon" vs "starved by scheduler") ----
    int setup_ms;             // total - dec - proc - emit (header parse, buffers, thread start, tail)
    int lut_ms;               // wall time resolving/building the 64x64x17 LUT (0 when cached)
    int cpu_dec_ms;           // Thread CPU time of Huffman decode (all threads)
    int cpu_s2_ms;            // Thread CPU time of Stage 2 (all workers incl. caller)
    int cpu_emit_ms;          // Thread CPU time of Stage 3 emit thread
    int idct_ms;              // Sampled (1/32 MCU) CPU ms spent in IDCT, extrapolated to whole image
    int lut_eval_ms;          // Sampled CPU ms in 3D LUT grading
    int fdct_ms;              // Sampled CPU ms in grain + FDCT + quantization
    int bench1;               // Calibration: M dependent-ops/s, 1 thread
    int bench_all;            // Calibration: M dependent-ops/s, ncpu threads at once
    int cur_mhz;              // cpu0 scaling_cur_freq in MHz (0 if unreadable)
    int bg_cgroup;            // 1 if the grading thread sits in a background cgroup
    int read_ms;              // Blocking stall ms waiting on async SD prefetcher
    int pin_cnt;              // Number of threads pinned to distinct CPU cores
    int dct_fast_pct;         // Percentage (0..100) of 8x8 luma blocks transcoded directly in DCT domain
    int open_errno;           // POSIX errno when output fopen() fails (rc == -9), else 0
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
