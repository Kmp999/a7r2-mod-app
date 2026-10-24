#ifndef FUJI_LOG_H
#define FUJI_LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Performance mandate: No logging in hot path by default
#ifdef FUJI_ENABLE_HOTPATH_LOGS
    #define FUJI_LOG_HOTPATH(fmt, ...) printf("[HOTPATH] " fmt "\n", ##__VA_ARGS__)
#else
    #define FUJI_LOG_HOTPATH(fmt, ...) do {} while (0)
#endif

#define FUJI_LOG_INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define FUJI_LOG_WARN(fmt, ...) printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define FUJI_LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // FUJI_LOG_H
