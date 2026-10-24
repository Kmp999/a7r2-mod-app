#ifndef FUJI_OUTPUT_WRITER_H
#define FUJI_OUTPUT_WRITER_H

#include <stdint.h>
#include <stddef.h>
#include "../util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* base_path;
    int is_initialized;
} OutputWriterConfig;

FujiResult output_writer_init(OutputWriterConfig* config, const char* mount_point);

// Synchronously or asynchronously writes an output buffer to disk
FujiResult output_writer_save_file(const OutputWriterConfig* config, const char* filename, const uint8_t* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // FUJI_OUTPUT_WRITER_H
