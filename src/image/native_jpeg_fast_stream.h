#ifndef NATIVE_JPEG_FAST_STREAM_H
#define NATIVE_JPEG_FAST_STREAM_H

#include "lut_cube_17.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pure-C Single-Pass 4-Core Parallel Streaming Baseline JPEG-to-JPEG Transcoder.
 *
 * Completely bypasses Java BitmapRegionDecoder (eliminating the 2.5s Huffman index scan,
 * 6.3s of per-strip seeks, Dalvik Bitmap allocations, and 71MB JNI pixel copies).
 *
 * Architecture:
 *  - Pre-bakes a 19.6KB L1-cache-resident 17^3 direct YCbCr -> YCbCr 3D LUT once per image
 *    (eliminating 100% of per-pixel YCbCr<->RGB conversions and evaluating chroma once per 2x2 quad).
 *  - Supports Sony BIONZ X native 4:2:2 (2x1), standard 4:2:0 (2x2), 4:4:4 (1x1), and 4:4:0 (1x2) SOF0 JPEGs.
 *  - Pre-loads standard Annex K Huffman tables before marker parsing and supports DRI restart intervals.
 *  - Supports Scale 1:1 (e.g. 5168x3448 -> 5168x3448) and 1:2 (e.g. 7952x5304 -> 3976x2652).
 *  - Emits clean sanitized EXIF APP1 (Orientation, DateTime, Exposure, FNumber, ISO, FocalLength)
 *    with standard JFIF APP0 and zero stale ungraded thumbnails / broken MakerNote offsets.
 *  - Scoped 4-core pthread fork-join (ZERO pthread_cond_wait) + overlapped async Huffman emit.
 *
 * Returns:
 *   1  on complete success
 *  <=0 on unsupported format or I/O error (negative code identifies exact failing stage for telemetry;
 *      -9 = output file open failed, triggers Tier 2/3 output path retry; other codes trigger clean BRD fallback).
 */
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
