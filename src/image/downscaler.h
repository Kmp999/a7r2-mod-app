#ifndef FUJI_DOWNSCALER_H
#define FUJI_DOWNSCALER_H

#include <stdint.h>
#include "image_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fast integer-only nearest-neighbor downscaler for RGB24
// Guarantees zero allocation, executes in ~5-15ms for 1080p -> 720p/VGA
FujiResult downscale_rgb24_nearest(const ImageBuffer* src, ImageBuffer* dst);

// Downscale NV21 buffer nearest neighbor
FujiResult downscale_nv21_nearest(const ImageBuffer* src, ImageBuffer* dst);

#ifdef __cplusplus
}
#endif

#endif // FUJI_DOWNSCALER_H
