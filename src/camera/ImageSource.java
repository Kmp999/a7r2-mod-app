package com.fujilite.a7r2;

/**
 * Image source abstraction defining how images are fed into the processing pipeline.
 */
public interface ImageSource {
    enum Type {
        CAMERA_HARDWARE_PREVIEW,
        CAMERA_JPEG_CAPTURE,
        TEST_SYNTHETIC_PATTERN
    }

    Type getType();
    int getWidth();
    int getHeight();
}
