package com.fujilite.a7r2;

import android.util.Log;
import java.nio.ByteBuffer;

/**
 * Lazy single-slot buffer for zero-copy image grading on Sony A7R II.
 * Allocated lazily on first capture (NEVER on app startup) to prevent Dalvik OOM on launch.
 */
public final class MemoryPool {
    private static final String TAG = "MemoryPool";

    public static final int BUFFER_CAPACITY = 22 * 1024 * 1024; // 22 MB (fits 10.6MP RGB_565 = ~21.1MB)
    public static final int BUFFER_CAPACITY_FALLBACK = 6 * 1024 * 1024; // 6 MB fallback (fits 2.6MP)

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
                Log.i(TAG, "Lazy direct grading buffer allocated (22 MB for 10.6MP)");
            } catch (Throwable t) {
                Log.w(TAG, "Failed to allocate 22MB direct buffer; attempting 6MB fallback", t);
                try {
                    sBuffer = ByteBuffer.allocateDirect(BUFFER_CAPACITY_FALLBACK);
                    Log.i(TAG, "Lazy direct grading buffer allocated (6 MB fallback)");
                } catch (Throwable t2) {
                    Log.e(TAG, "Failed to allocate fallback direct buffer", t2);
                    return null;
                }
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
