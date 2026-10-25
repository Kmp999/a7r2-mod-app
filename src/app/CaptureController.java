package com.fujilite.a7r2;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.hardware.Camera;
import android.os.Environment;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ShortBuffer;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Manages the capture pipeline, dispatching image processing off the UI thread.
 * Decodes proxy JPEG to RGB_565, grades with 17x17x17 3D LUT (native or Java fallback),
 * then re-encodes and writes to SD card asynchronously.
 */
public class CaptureController {
    private static final String TAG = "CaptureController";

    // Use native 17x17x17 3D LUT when available, gracefully fallback to Java engine
    private static final boolean USE_NATIVE_ENGINE = true;

    // Heap-safe pixel budget (4 MP for smooth turnaround under 500ms on A7R II)
    private static final int MAX_GRADE_PIXELS = 4 * 1024 * 1024;

    private static final String OUTPUT_DIR = "/sdcard/DCIM/100MSDCF/";

    private final CameraAdapter cameraAdapter;
    private final UIController uiController;
    private final AppState appState;
    private final ExecutorService processingExecutor;
    private final JavaColorEngine javaEngine = new JavaColorEngine();

    public CaptureController(CameraAdapter adapter, UIController uiController, AppState state) {
        this.cameraAdapter = adapter;
        this.uiController = uiController;
        this.appState = state;
        this.processingExecutor = Executors.newSingleThreadExecutor();
    }

    /** Delivers the graded bitmap to the UI; the UI owns and recycles it. */
    public interface ResultListener {
        void onResult(Bitmap graded, String info);
    }

    private volatile ResultListener resultListener;

    public void setResultListener(ResultListener l) {
        this.resultListener = l;
    }

    public void triggerCapture() {
        if (appState.isProcessing()) {
            return;
        }
        appState.setProcessing(true);
        uiController.updateStatusAsync("Capturing...");

        cameraAdapter.takePicture(new Camera.PictureCallback() {
            @Override
            public void onPictureTaken(final byte[] jpegData, Camera camera) {
                processingExecutor.execute(new Runnable() {
                    @Override
                    public void run() {
                        processJpeg(jpegData);
                    }
                });
            }
        });
    }

    private void processJpeg(byte[] jpegData) {
        final long start = System.currentTimeMillis();
        try {
            if (jpegData == null || jpegData.length == 0) {
                uiController.updateStatusAsync("Capture failed: Empty buffer");
                return;
            }

            // 1. Probe dimensions without allocating the full bitmap
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(jpegData, 0, jpegData.length, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
                uiController.updateStatusAsync("Capture failed: Not a JPEG");
                return;
            }

            // 2. Decode as RGB_565 at the largest heap-safe scale
            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inPreferredConfig = Bitmap.Config.RGB_565;
            opts.inSampleSize = computeSampleSize(bounds.outWidth, bounds.outHeight, MAX_GRADE_PIXELS);
            Bitmap bitmap = BitmapFactory.decodeByteArray(jpegData, 0, jpegData.length, opts);
            if (bitmap == null) {
                uiController.updateStatusAsync("Capture failed: decode error");
                return;
            }

            int w = bitmap.getWidth();
            int h = bitmap.getHeight();

            // 3. Color grade (RGB_565 short[])
            boolean graded = false;
            short[] pixels = new short[w * h];
            ShortBuffer buffer = ShortBuffer.wrap(pixels);
            bitmap.copyPixelsToBuffer(buffer);

            if (USE_NATIVE_ENGINE && ColorEngine.isNativeAvailable()) {
                FujiRecipe recipe = appState.getActiveRecipe();
                ColorEngine.precomputeLut17Safe(
                    recipe.computeCcmQ8(),
                    recipe.generateToneCurve('R'),
                    recipe.generateToneCurve('G'),
                    recipe.generateToneCurve('B')
                );
                graded = ColorEngine.applyLut17RGB565Safe(pixels, w * h);
            }

            if (!graded) {
                javaEngine.init(appState.getCurrentPresetId());
                javaEngine.processRGB565(pixels, w, h);
                graded = true;
            }

            buffer.rewind();
            bitmap.copyPixelsFromBuffer(buffer);

            // 4. Resume live view immediately, then save and show the result
            cameraAdapter.resumePreview();
            String savedPath = saveJpeg(bitmap);
            long duration = System.currentTimeMillis() - start;
            appState.setLastProcessingTimeMs(duration);
            deliverResult(bitmap, appState.getCurrentPresetName(), w, h, duration, savedPath);
        } catch (Throwable t) {
            Log.e(TAG, "Error processing capture", t);
            appState.setLastErrorMessage(String.valueOf(t.getMessage()));
            uiController.updateStatusAsync("Error: " + t.getClass().getSimpleName());
        } finally {
            appState.setProcessing(false);
        }
    }

    private void deliverResult(Bitmap bitmap, String preset, int w, int h, long ms, String savedPath) {
        uiController.updateStatusAsync(preset + "  " + w + "x" + h + "  " + ms + " ms"
            + (savedPath == null ? "  SAVE FAILED" : ""));
        ResultListener l = resultListener;
        if (l != null) {
            String where = savedPath != null ? ("Saved: " + savedPath) : "Save failed - check SD card";
            l.onResult(bitmap, preset + "   " + w + "x" + h + "   " + ms + " ms\n" + where);
        } else {
            bitmap.recycle();
        }
    }

    /**
     * Renders a synthetic test chart and grades it with the selected preset.
     */
    public void runDemo() {
        if (appState.isProcessing()) return;
        appState.setProcessing(true);
        uiController.updateStatusAsync("Rendering demo...");
        processingExecutor.execute(new Runnable() {
            @Override
            public void run() {
                final long start = System.currentTimeMillis();
                try {
                    Bitmap bitmap = makeTestImage(1280, 720);
                    int w = bitmap.getWidth();
                    int h = bitmap.getHeight();
                    short[] pixels = new short[w * h];
                    ShortBuffer buffer = ShortBuffer.wrap(pixels);
                    bitmap.copyPixelsToBuffer(buffer);

                    boolean graded = false;
                    if (USE_NATIVE_ENGINE && ColorEngine.isNativeAvailable()) {
                        FujiRecipe recipe = appState.getActiveRecipe();
                        ColorEngine.precomputeLut17Safe(
                            recipe.computeCcmQ8(),
                            recipe.generateToneCurve('R'),
                            recipe.generateToneCurve('G'),
                            recipe.generateToneCurve('B')
                        );
                        graded = ColorEngine.applyLut17RGB565Safe(pixels, w * h);
                    }
                    if (!graded) {
                        javaEngine.init(appState.getCurrentPresetId());
                        javaEngine.processRGB565(pixels, w, h);
                        graded = true;
                    }

                    buffer.rewind();
                    bitmap.copyPixelsFromBuffer(buffer);
                    String savedPath = saveJpeg(bitmap);
                    long duration = System.currentTimeMillis() - start;
                    deliverResult(bitmap, appState.getCurrentPresetName() + " (demo)", w, h, duration, savedPath);
                } catch (Throwable t) {
                    Log.e(TAG, "Demo failed", t);
                    uiController.updateStatusAsync("Demo error: " + t.getClass().getSimpleName());
                } finally {
                    appState.setProcessing(false);
                }
            }
        });
    }

    private Bitmap makeTestImage(int w, int h) {
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.RGB_565);
        Canvas c = new Canvas(bmp);
        Paint p = new Paint();
        int[] bars = {
            Color.rgb(255, 255, 255), Color.rgb(255, 0, 0), Color.rgb(0, 255, 0),
            Color.rgb(0, 0, 255), Color.rgb(255, 255, 0), Color.rgb(0, 255, 255),
            Color.rgb(255, 0, 255), Color.rgb(128, 128, 128)
        };
        int bw = w / bars.length;
        for (int i = 0; i < bars.length; i++) {
            p.setColor(bars[i]);
            c.drawRect(i * bw, 0, (i + 1) * bw, h / 2, p);
        }
        for (int x = 0; x < w; x++) {
            int v = (int) (255L * x / w);
            p.setColor(Color.rgb(v, v, v));
            c.drawRect(x, h / 2, x + 1, h, p);
        }
        return bmp;
    }

    /** Largest power-of-two sample size that keeps decoded pixel count within budget. */
    static int computeSampleSize(int width, int height, int maxPixels) {
        int sample = 1;
        while (((long) (width / sample)) * (height / sample) > maxPixels) {
            sample *= 2;
        }
        return sample;
    }

    /** Saves graded JPEG to SD-card. Returns path or null. */
    private String saveJpeg(Bitmap bitmap) {
        String name = "FUJI_" + System.currentTimeMillis() + ".JPG";
        String ext = Environment.getExternalStorageDirectory() + "/DCIM/100MSDCF/";
        String[] dirs = { OUTPUT_DIR, ext, "/sdcard/DCIM/", "/storage/sdcard1/DCIM/100MSDCF/" };
        for (String d : dirs) {
            FileOutputStream out = null;
            try {
                File dir = new File(d);
                if (!dir.exists()) {
                    dir.mkdirs();
                }
                File file = new File(dir, name);
                out = new FileOutputStream(file);
                if (bitmap.compress(Bitmap.CompressFormat.JPEG, 92, out)) {
                    out.flush();
                    return file.getAbsolutePath();
                }
            } catch (Throwable t) {
                Log.e(TAG, "Save failed in " + d, t);
            } finally {
                if (out != null) {
                    try {
                        out.close();
                    } catch (Throwable ignored) {
                    }
                }
            }
        }
        return null;
    }

    public void shutdown() {
        processingExecutor.shutdown();
    }
}
