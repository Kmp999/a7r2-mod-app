#ifndef FUJI_TIMESTAMP_H
#define FUJI_TIMESTAMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// High-resolution monotonic timestamp in microseconds
uint64_t fuji_get_timestamp_us(void);

// High-resolution timestamp in milliseconds
uint64_t fuji_get_timestamp_ms(void);

#ifdef __cplusplus
}
#endif

#endif // FUJI_TIMESTAMP_H
