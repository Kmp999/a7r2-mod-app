package com.fujilite.a7r2;

import android.util.Log;
import java.nio.ByteBuffer;

/**
 * Lazy single-slot buffer for zero-copy image grading on Sony A7R II.
 * Allocated lazily on first capture (NEVER on app startup) to prevent Dalvik OOM on launch.
 */
public final class MemoryPool {
    private static final String TAG = "MemoryPool";

    // NO 22MB BUFFER: We stream 10.6MP directly to the JPEG encoder in 16-row strips (127KB)
    public static final int BUFFER_CAPACITY = 22 * 1024 * 1024; // Deprecated: eliminated in favor of 127KB strip streaming
    public static final int PROXY_CAPACITY = 4 * 1024 * 1024; // 4 MB (fits 1616x1080 RGB_565 = ~3.5MB)

    private static ByteBuffer sProxyBuffer = null;
    private static volatile boolean sProxyInUse = false;

    public static synchronized void initPool() {
        // No-op: memory is allocated lazily on demand to guarantee zero startup crash
    }

    public static synchronized ByteBuffer acquireProxySlot() {
        if (sProxyInUse) return null;
        if (sProxyBuffer == null) {
            try {
                sProxyBuffer = ByteBuffer.allocateDirect(PROXY_CAPACITY);
                Log.i(TAG, "Lazy direct proxy buffer allocated (4 MB for 1.7MP)");
            } catch (Throwable t) {
                Log.e(TAG, "Failed to allocate 4MB proxy direct buffer", t);
                return null;
            }
        }
        sProxyInUse = true;
        sProxyBuffer.clear();
        return sProxyBuffer;
    }

    public static synchronized void releaseProxySlot(ByteBuffer buffer) {
        if (buffer == sProxyBuffer) {
            sProxyInUse = false;
        }
    }

    // Deprecated methods kept to prevent LinkageError, they now return proxy or null
    public static synchronized ByteBuffer acquireFreeSlot() {
        Log.w(TAG, "acquireFreeSlot() called, but 22MB buffer is eliminated. Use proxy slot.");
        return null;
    }

    public static void markForIO(ByteBuffer buffer) {}

    public static synchronized void releaseSlot(ByteBuffer buffer) {
        if (buffer == sProxyBuffer) {
            sProxyInUse = false;
        }
    }

    private MemoryPool() {}
}
