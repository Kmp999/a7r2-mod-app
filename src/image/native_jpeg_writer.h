#ifndef NATIVE_JPEG_WRITER_H
#define NATIVE_JPEG_WRITER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JpegStreamState JpegStreamState;

/**
 * Writes an RGB565 image directly to a JPEG file on disk in one shot.
 * High-performance baseline JPEG encoder (4:2:0 chroma subsampling)
 * with integer fixed-point FDCT.
 *
 * @param filename Output file path
 * @param rgb565   Pointer to packed RGB_565 pixel buffer
 * @param width    Width in pixels
 * @param height   Height in pixels
 * @param quality  JPEG Quality (1 - 100, e.g. 95)
 * @return 1 on success, 0 on failure
 */
int native_write_jpeg_rgb565(const char* filename, const uint16_t* rgb565, int width, int height, int quality);

/**
 * Opens a streaming JPEG writer session for writing full-resolution (42.4MP: 7952x5304) JPEGs in strips.
 * Writes standard JFIF headers (SOI, DQT, SOF0, DHT, SOS) and prepares buffered BitWriter.
 *
 * @param filename Target file path (e.g. /mnt/sdcard/DCIM/100MSDCF/fuji_0001.jpg.tmp)
 * @param width    Target full width (e.g. 7952)
 * @param height   Target full height (e.g. 5304)
 * @param quality  JPEG quality (1 - 100, e.g. 95)
 * @return Opaque stream pointer or NULL on failure
 */
JpegStreamState* native_jpeg_stream_open(const char* filename, int width, int height, int quality);

/**
 * Encodes a strip of RGB565 rows into the active JPEG stream.
 *
 * @param state        Active stream session
 * @param strip_pixels Pointer to RGB_565 strip buffer
 * @param strip_w      Strip width (must match stream width)
 * @param strip_h      Strip height in rows (e.g. 64)
 * @param row_offset   Starting row offset in full image (e.g. 0, 64, 128...)
 * @return 1 on success, 0 on failure
 */
int native_jpeg_stream_write_strip_rgb565(JpegStreamState* state, const uint16_t* strip_pixels, int strip_w, int strip_h, int row_offset);

/**
 * Closes the JPEG stream session.
 * If success != 0, flushes bit buffer, writes EOI marker (0xFF, 0xD9), and closes file.
 * If success == 0, aborts and cleans up.
 *
 * @param state   Active stream session
 * @param success 1 to finalize normally, 0 to abort
 * @return 1 on success, 0 on failure
 */
int native_jpeg_stream_close(JpegStreamState* state, int success);

#ifdef __cplusplus
}
#endif

#endif // NATIVE_JPEG_WRITER_H
