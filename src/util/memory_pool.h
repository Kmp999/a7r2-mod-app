#ifndef FUJI_MEMORY_POOL_H
#define FUJI_MEMORY_POOL_H

#include <stddef.h>
#include <stdint.h>
#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max buffer size: 1920x1080 * 3 bytes (RGB) = 6.22 MB
// Pre-allocated statically at startup to avoid runtime malloc/free in Dalvik/NDK
#define FUJI_POOL_MAX_BUFFER_SIZE (1920 * 1080 * 4)

typedef struct {
    uint8_t static_buffer_a[FUJI_POOL_MAX_BUFFER_SIZE];
    uint8_t static_buffer_b[FUJI_POOL_MAX_BUFFER_SIZE];
    int buffer_a_in_use;
    int buffer_b_in_use;
} MemoryPool;

FujiResult memory_pool_init(MemoryPool* pool);
uint8_t* memory_pool_acquire_buffer(MemoryPool* pool, size_t required_size);
void memory_pool_release_buffer(MemoryPool* pool, uint8_t* buffer);

#ifdef __cplusplus
}
#endif

#endif // FUJI_MEMORY_POOL_H
