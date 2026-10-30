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
    ../src/perf/timestamp.c \
    ../src/perf/profiler.c \
    ../src/util/memory_pool.c

LOCAL_LDLIBS    := -llog -lm -ljnigraphics

# Optimization flags for ARMv7-A (Sony A7R II BIONZ X Cortex-A CPU)
LOCAL_CFLAGS    := -O3 -mfloat-abi=softfp -mfpu=neon -ffast-math -ftree-vectorize

include $(BUILD_SHARED_LIBRARY)
