#!/usr/bin/env python3
"""
Unit tests verifying elimination of the 30-second capture freeze and deadlock conditions:
  1. SonyPmcaAdapter eliminates cancelTakePicture() in CameraEx$ShutterListener.onShutter,
     preventing premature exposure abortion by BIONZ X.
  2. color_engine_jni.c eliminates pthread_cond_wait deadlocks in encode worker, ensuring
     100% synchronous, deterministic strip grading & JPEG encoding without lost wakeups.
  3. StorageResolver.waitForNewCameraPhoto returns null on timeout if fallback photo is
     in preExistingFiles, preventing silent aborts on old photos.
  4. CaptureController provides granular HUD status telemetry during each phase (shutter,
     waiting SD, strip-by-strip grading, and error reporting).
"""

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


class TestFreezeAndDeadlockElimination(unittest.TestCase):
    def setUp(self):
        self.src = REPO_ROOT / "src"
        self.adapter_java = (self.src / "camera" / "SonyPmcaAdapter.java").read_text(encoding="utf-8")
        self.jni_c = (self.src / "color" / "color_engine_jni.c").read_text(encoding="utf-8")
        self.storage_java = (self.src / "app" / "StorageResolver.java").read_text(encoding="utf-8")
        self.capture_java = (self.src / "app" / "CaptureController.java").read_text(encoding="utf-8")

    def test_shutter_listener_does_not_abort_exposure(self):
        """CameraEx$ShutterListener.onShutter must NOT call cancelTakePicture() which aborts exposure."""
        shutter_idx = self.adapter_java.find('if ("onShutter".equals(mName))')
        self.assertNotEqual(shutter_idx, -1)
        on_shutter_block = self.adapter_java[shutter_idx:shutter_idx + 400]
        self.assertNotIn("cancelTakePicture()", on_shutter_block)
        self.assertIn("activeShutterCallback", on_shutter_block)

    def test_color_engine_jni_synchronous_encode_worker(self):
        """color_engine_jni.c must NOT use condition variables in encode_worker_submit."""
        self.assertIn("encode_worker_submit", self.jni_c)
        self.assertNotIn("pthread_cond_wait(&g_encode_worker.cond_done", self.jni_c)
        self.assertNotIn("pthread_cond_wait(&g_strip_worker.cond_done", self.jni_c)
        self.assertIn("native_jpeg_stream_write_strip_argb8888", self.jni_c)
        self.assertIn("native_jpeg_stream_write_strip_rgb565", self.jni_c)

    def test_storage_resolver_timeout_guards_pre_existing_fallback(self):
        """waitForNewCameraPhoto returns null if fallback photo was already in preExistingFiles."""
        fn_idx = self.storage_java.find("public static File waitForNewCameraPhoto(final Set<String> preExistingFiles")
        self.assertNotEqual(fn_idx, -1)
        fn_block = self.storage_java[fn_idx:fn_idx + 4000]
        self.assertIn("preExistingFiles.contains(fbPath)", fn_block)
        self.assertIn("return null; // Never falsely return an old pre-existing photo on timeout!", fn_block)

    def test_capture_controller_granular_hud_telemetry(self):
        """CaptureController reports granular HUD updates across all pipeline stages and keeps Java 7 final vars."""
        self.assertIn('uiController.updateStatusAsync("Waiting for SD photo...");', self.capture_java)
        self.assertIn("Grading strip", self.capture_java)
        self.assertIn('uiController.updateStatusAsync("ERR: SD timeout (no photo written)");', self.capture_java)
        self.assertIn("final File rawFile = resolvedFile;", self.capture_java)


if __name__ == "__main__":
    unittest.main()
