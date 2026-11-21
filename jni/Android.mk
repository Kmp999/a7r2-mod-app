LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := color_engine_jni

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/../src/color \
    $(LOCAL_PATH)/../src/color/presets \
    $(LOCAL_PATH)/../src/image \
    $(LOCAL_PATH)/../src/perf \
    $(LOCAL_PATH)/../src/util

LOCAL_SRC_FILES := \
    ../src/color/color_engine.c \
    ../src/color/color_engine_jni.c \
    ../src/color/preset_manager.c \
    ../src/color/tone_curve_lut.c \
    ../src/color/presets/provia_like.c \
    ../src/color/presets/astia_like.c \
    ../src/color/presets/velvia_like.c \
    ../src/color/presets/classic_neg_like.c \
    ../src/color/presets/eterna_like.c \
    ../src/image/image_buffer.c \
    ../src/image/downscaler.c \
    ../src/image/native_jpeg_writer.c \
    ../src/image/native_jpeg_fast_stream.c \
    ../src/perf/timestamp.c \
    ../src/perf/profiler.c \
    ../src/util/memory_pool.c

LOCAL_LDLIBS    := -llog -lm

# Compatible with Sony A7R II (ILCE-7RM2) BIONZ X CXD90014 ARM Cortex-A5 (Android 2.3.7/4.1.2).
# Force full 32-bit ARM mode (prevents 16-bit Thumb-1 register spilling -> 3-5x faster IDCT/FDCT/LUT).
# Avoids forced NEON (prevents SIGILL on non-NEON cores) and emits classic DT_HASH for legacy dlopen.
LOCAL_ARM_MODE  := arm
LOCAL_CFLAGS    := -std=gnu99 -O3 -funroll-loops -fomit-frame-pointer -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
LOCAL_SRC_FILES += ../src/image/neon_jpeg_kernels.c.neon
LOCAL_CFLAGS    += -mcpu=cortex-a5 -mtune=cortex-a5 -DHAVE_NEON_KERNELS=1
endif
LOCAL_LDFLAGS   := -Wl,--hash-style=both

include $(BUILD_SHARED_LIBRARY)
