#include "output_writer.h"
#include <stdio.h>
#include <string.h>

FujiResult output_writer_init(OutputWriterConfig* config, const char* mount_point) {
    if (!config) return FUJI_ERR_NULL_POINTER;
    config->base_path = mount_point ? mount_point : "/sdcard/DCIM/100MSDCF/";
    config->is_initialized = 1;
    return FUJI_SUCCESS;
}

FujiResult output_writer_save_file(const OutputWriterConfig* config, const char* filename, const uint8_t* data, size_t size) {
    if (!config || !filename || !data || size == 0) return FUJI_ERR_NULL_POINTER;

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", config->base_path, filename);

    FILE* fp = fopen(full_path, "wb");
    if (!fp) {
        return FUJI_ERR_IO;
    }

    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);

    if (written != size) {
        return FUJI_ERR_IO;
    }

    return FUJI_SUCCESS;
}
