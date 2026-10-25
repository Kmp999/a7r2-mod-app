#include "memory_pool.h"

FujiResult memory_pool_init(MemoryPool* pool) {
    if (!pool) return FUJI_ERR_NULL_POINTER;
    pool->slots[0].state = FUJI_SLOT_FREE;
    pool->slots[1].state = FUJI_SLOT_FREE;
    pool->slots[2].state = FUJI_SLOT_FREE;
    return FUJI_SUCCESS;
}

uint8_t* memory_pool_acquire_buffer(MemoryPool* pool, size_t required_size) {
    if (!pool || required_size > FUJI_POOL_MAX_BUFFER_SIZE) {
        return NULL;
    }
    int i;
    for (i = 0; i < FUJI_POOL_NUM_SLOTS; i++) {
        if (pool->slots[i].state == FUJI_SLOT_FREE) {
            pool->slots[i].state = FUJI_SLOT_DECODING_GRADING;
            return pool->slots[i].buffer;
        }
    }
    return NULL; // All 3 slots currently in use
}

void memory_pool_mark_io(MemoryPool* pool, uint8_t* buffer) {
    if (!pool || !buffer) return;
    int i;
    for (i = 0; i < FUJI_POOL_NUM_SLOTS; i++) {
        if (pool->slots[i].buffer == buffer) {
            pool->slots[i].state = FUJI_SLOT_IO_WRITING;
            return;
        }
    }
}

void memory_pool_release_buffer(MemoryPool* pool, uint8_t* buffer) {
    if (!pool || !buffer) return;
    int i;
    for (i = 0; i < FUJI_POOL_NUM_SLOTS; i++) {
        if (pool->slots[i].buffer == buffer) {
            pool->slots[i].state = FUJI_SLOT_FREE;
            return;
        }
    }
}
