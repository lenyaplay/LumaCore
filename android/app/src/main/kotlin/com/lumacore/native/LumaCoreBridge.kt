package com.lumacore.native

import android.view.Surface

/**
 * Thin JNI wrapper only — no logic here, everything forwards to
 * lumacore_api.h via native/jni/lumacore_jni.cpp. See ARCHITECTURE.md §5,
 * ai_plans/04-android-camerax-gl-pipeline.md §A.3/§C.
 *
 * setEffectParams/getStats are deliberately NOT exposed here — they go
 * through dart:ffi directly into liblumacore.so on Android too (see
 * lib/core/ffi/lumacore_bindings.dart), mirroring iOS's Obj-C++
 * LumaCoreBridge, which also has no such methods.
 */
class LumaCoreBridge {
    external fun nativeRenderInit(surface: Long, width: Int, height: Int): Long
    external fun nativeRelease(session: Long)

    // Converts a Flutter-provided android.view.Surface into the ANativeWindow*
    // (as a jlong) nativeRenderInit's `surface` param expects — Kotlin has no
    // way to obtain that pointer itself (ANativeWindow_fromSurface is an NDK
    // function, native-only). The returned pointer's +1 reference is released
    // by GLRenderBackend::teardown(), not here.
    external fun nativeAcquireWindow(surface: Surface): Long

    // Android push-model counterpart to iOS's CVPixelBufferRef import: the
    // GL_TEXTURE_EXTERNAL_OES texture name the camera-input SurfaceTexture
    // must wrap. -1 if session is invalid.
    external fun nativeGetCameraTextureId(session: Long): Int

    external fun nativeSetThermalState(session: Long, state: Int)

    // SurfaceTexture.getTransformMatrix()'s 4x4 column-major matrix, refreshed
    // every frame right after updateTexImage() — corrects for the camera
    // sensor's native buffer orientation (rotation/mirroring), which the raw
    // external-OES sample would otherwise show uncorrected.
    external fun nativeSetCameraTransform(session: Long, matrix: FloatArray)
    external fun nativeStartRecording(session: Long, outPath: String, bitrateKbps: Int, width: Int, height: Int): Int

    // No cameraFrame/outPreviewImage params unlike iOS's render call — the
    // camera frame is already bound into the GL external-OES texture via
    // SurfaceTexture.updateTexImage() before this is called, and the
    // rendered frame is presented directly (eglSwapBuffers inside native
    // code), not handed back as a CPU buffer.
    external fun nativeRenderFrame(session: Long, ptsUs: Long): Int

    external fun nativeStopRecording(session: Long): Int
    external fun nativeSubmitAudioFrame(
        session: Long,
        pcmData: ByteArray,
        numFrames: Int,
        sampleRate: Int,
        numChannels: Int,
        ptsUs: Long,
    ): Int

    external fun nativeValidateLicense(tokenBlobJson: String, deviceFingerprint: String): Int

    companion object {
        init {
            System.loadLibrary("lumacore")
        }
    }
}
