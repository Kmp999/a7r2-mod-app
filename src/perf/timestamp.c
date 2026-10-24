#include "timestamp.h"

#if defined(_WIN32)
#include <windows.h>
uint64_t fuji_get_timestamp_us(void) {
    LARGE_INTEGER count, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)((count.QuadPart * 1000000) / freq.QuadPart);
}
#else
#include <time.h>
uint64_t fuji_get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

uint64_t fuji_get_timestamp_ms(void) {
    return fuji_get_timestamp_us() / 1000ULL;
}
