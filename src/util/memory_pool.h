#ifndef FUJI_MEMORY_POOL_H
#define FUJI_MEMORY_POOL_H

#include <stddef.h>
#include <stdint.h>
#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max buffer size: 1920x1080 * 2 bytes (RGB_565) = 4.14 MB
#define FUJI_POOL_MAX_BUFFER_SIZE (1920 * 1080 * 2)
#define FUJI_POOL_NUM_SLOTS 3

typedef enum {
    FUJI_SLOT_FREE = 0,
    FUJI_SLOT_DECODING_GRADING = 1,
    FUJI_SLOT_IO_WRITING = 2
} FujiSlotState;

typedef struct {
    uint8_t buffer[FUJI_POOL_MAX_BUFFER_SIZE];
    FujiSlotState state;
} FujiMemorySlot;

typedef struct {
    FujiMemorySlot slots[FUJI_POOL_NUM_SLOTS];
} MemoryPool;

FujiResult memory_pool_init(MemoryPool* pool);
uint8_t* memory_pool_acquire_buffer(MemoryPool* pool, size_t required_size);
void memory_pool_mark_io(MemoryPool* pool, uint8_t* buffer);
void memory_pool_release_buffer(MemoryPool* pool, uint8_t* buffer);

#ifdef __cplusplus
}
#endif

#endif // FUJI_MEMORY_POOL_H
