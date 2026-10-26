package com.fujilite.a7r2;

import android.util.Log;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Triple-Slot Ring Buffer Memory Pool for zero-copy image grading on Sony A7R II.
 *
 * Provides 3 pre-allocated direct ByteBuffers:
 *  - Slot 0, 1, 2: 4MB each (sufficient for 1080p / 4MP in RGB_565).
 * States:
 *  - 0: FREE
 *  - 1: DECODING_GRADING (occupied by active JNI color transformation ~48ms)
 *  - 2: IO_WRITING (occupied by asynchronous background SD card write ~240ms)
 *
 * Completely eliminates runtime malloc / Dalvik GC thrashing.
 */
public final class MemoryPool {
    private static final String TAG = "MemoryPool";

    public static final int SLOT_COUNT = 3;
    public static final int BUFFER_CAPACITY = 12 * 1024 * 1024; // 12 MB in Native C RAM

    public static final int STATE_FREE = 0;
    public static final int STATE_DECODING_GRADING = 1;
    public static final int STATE_IO_WRITING = 2;

    private static final ByteBuffer[] sBuffers = new ByteBuffer[SLOT_COUNT];
    private static final AtomicInteger[] sStates = new AtomicInteger[SLOT_COUNT];
    private static volatile boolean sInitialized = false;

    private static synchronized void ensureInitialized() {
        if (sInitialized) return;
        try {
            for (int i = 0; i < SLOT_COUNT; i++) {
                sBuffers[i] = ByteBuffer.allocateDirect(BUFFER_CAPACITY);
                sStates[i] = new AtomicInteger(STATE_FREE);
            }
            sInitialized = true;
            Log.i(TAG, "Triple-slot direct memory pool initialized (3 x 4MB)");
        } catch (Throwable t) {
            Log.e(TAG, "Failed to allocate direct memory pool", t);
        }
    }

    public static void initPool() {
        ensureInitialized();
    }

    /**
     * Acquires the first available FREE slot and transitions it to DECODING_GRADING.
     * Returns null if all 3 slots are congested.
     */
    public static ByteBuffer acquireFreeSlot() {
        ensureInitialized();
        if (!sInitialized) return null;

        for (int i = 0; i < SLOT_COUNT; i++) {
            if (sStates[i].compareAndSet(STATE_FREE, STATE_DECODING_GRADING)) {
                ByteBuffer buf = sBuffers[i];
                buf.clear();
                return buf;
            }
        }

        Log.w(TAG, "All 3 memory pool slots are currently busy!");
        return null;
    }

    /**
     * Transitions an active buffer from DECODING_GRADING to IO_WRITING.
     */
    public static void markForIO(ByteBuffer buffer) {
        if (buffer == null || !sInitialized) return;
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (sBuffers[i] == buffer) {
                sStates[i].set(STATE_IO_WRITING);
                return;
            }
        }
    }

    /**
     * Releases a buffer back to the FREE state.
     */
    public static void releaseSlot(ByteBuffer buffer) {
        if (buffer == null || !sInitialized) return;
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (sBuffers[i] == buffer) {
                sStates[i].set(STATE_FREE);
                return;
            }
        }
    }

    private MemoryPool() {}
}
