#include "memory_pool.h"

FujiResult memory_pool_init(MemoryPool* pool) {
    if (!pool) return FUJI_ERR_NULL_POINTER;
    pool->buffer_a_in_use = 0;
    pool->buffer_b_in_use = 0;
    return FUJI_SUCCESS;
}

uint8_t* memory_pool_acquire_buffer(MemoryPool* pool, size_t required_size) {
    if (!pool || required_size > FUJI_POOL_MAX_BUFFER_SIZE) {
        return NULL;
    }
    if (!pool->buffer_a_in_use) {
        pool->buffer_a_in_use = 1;
        return pool->static_buffer_a;
    }
    if (!pool->buffer_b_in_use) {
        pool->buffer_b_in_use = 1;
        return pool->static_buffer_b;
    }
    return NULL; // Out of pooled static buffers
}

void memory_pool_release_buffer(MemoryPool* pool, uint8_t* buffer) {
    if (!pool || !buffer) return;
    if (buffer == pool->static_buffer_a) {
        pool->buffer_a_in_use = 0;
    } else if (buffer == pool->static_buffer_b) {
        pool->buffer_b_in_use = 0;
    }
}
