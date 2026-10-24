#ifndef FUJI_ERROR_H
#define FUJI_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FUJI_SUCCESS = 0,
    FUJI_ERR_NULL_POINTER = -1,
    FUJI_ERR_INVALID_PARAM = -2,
    FUJI_ERR_OUT_OF_MEMORY = -3,
    FUJI_ERR_TIMEOUT = -4,
    FUJI_ERR_IO = -5,
    FUJI_ERR_OVERFLOW = -6,
    FUJI_ERR_UNSUPPORTED = -7
} FujiResult;

#ifdef __cplusplus
}
#endif

#endif // FUJI_ERROR_H
