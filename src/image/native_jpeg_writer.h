#ifndef NATIVE_JPEG_WRITER_H
#define NATIVE_JPEG_WRITER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Writes an RGB565 image directly to a JPEG file on disk.
 * High-performance baseline JPEG encoder (4:2:0 chroma subsampling)
 * with integer fixed-point FDCT. Zero dynamic allocations.
 *
 * @param filename Output file path (e.g. /mnt/sdcard/DCIM/100MSDCF/FUJI0001.JPG)
 * @param rgb565   Pointer to packed RGB_565 pixel buffer
 * @param width    Width in pixels (e.g. 3984)
 * @param height   Height in pixels (e.g. 2656)
 * @param quality  JPEG Quality (1 - 100, e.g. 94)
 * @return 1 on success, 0 on failure
 */
int native_write_jpeg_rgb565(const char* filename, const uint16_t* rgb565, int width, int height, int quality);

/**
 * Strip-streaming edge-directed upscaler + JPEG encoder.
 * Upscales 16 rows at a time and encodes immediately, eliminating the need
 * for a 22MB intermediate buffer.
 *
 * @param filename Target file path
 * @param src_pixels Source pixels in RGB565 (typically 1616x1080)
 * @param srcW Source width
 * @param srcH Source height
 * @param dstW Target width (typically 3984)
 * @param dstH Target height (typically 2656)
 * @param quality JPEG quality
 * @return 1 on success, 0 on failure
 */
int native_upscale_and_write_jpeg_rgb565(const char* filename, const uint16_t* src_pixels, int srcW, int srcH, int dstW, int dstH, int quality);

#ifdef __cplusplus
}
#endif

#endif // NATIVE_JPEG_WRITER_H
