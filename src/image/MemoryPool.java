package com.fujilite.a7r2;

import android.util.Log;
import java.nio.ByteBuffer;

/**
 * Lazy single-slot buffer for zero-copy image grading on Sony A7R II.
 * Allocated lazily on first capture (NEVER on app startup) to prevent Dalvik OOM on launch.
 */
public final class MemoryPool {
    private static final String TAG = "MemoryPool";

    public static final int BUFFER_CAPACITY = 6 * 1024 * 1024; // 6 MB (fits 2.6MP RGB_565 = ~5.3MB)

    private static ByteBuffer sBuffer = null;
    private static volatile boolean sInUse = false;

    public static synchronized void initPool() {
        // No-op: memory is allocated lazily on demand to guarantee zero startup crash
    }

    public static synchronized ByteBuffer acquireFreeSlot() {
        if (sInUse) return null;
        if (sBuffer == null) {
            try {
                sBuffer = ByteBuffer.allocateDirect(BUFFER_CAPACITY);
                Log.i(TAG, "Lazy direct grading buffer allocated (6 MB)");
            } catch (Throwable t) {
                Log.e(TAG, "Failed to allocate direct buffer", t);
                return null;
            }
        }
        sInUse = true;
        sBuffer.clear();
        return sBuffer;
    }

    public static void markForIO(ByteBuffer buffer) {
        // No-op
    }

    public static synchronized void releaseSlot(ByteBuffer buffer) {
        sInUse = false;
    }

    private MemoryPool() {}
}
