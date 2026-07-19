#ifdef __ANDROID__

#include <jni.h>

#include <android/native_window_jni.h>

#include <cstdint>
#include <vector>

#include "api/lumacore_api.h"

// Thin JNIEXPORT layer only — no logic here, everything forwards to
// lumacore_api.h. See ARCHITECTURE.md §5 (jni/ is a language-boundary shim,
// not a place for behavior), ai_plans/04-android-camerax-gl-pipeline.md §A.3.
//
// setEffectParams/getStats are deliberately NOT exposed here — they go
// through dart:ffi directly into liblumacore.so on Android too, same as
// iOS's DynamicLibrary.process() path (see
// lib/core/ffi/lumacore_bindings.dart), matching the existing
// Obj-C++ LumaCoreBridge, which also has no such methods.

extern "C" JNIEXPORT jlong JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeRenderInit(JNIEnv*, jobject, jlong surface, jint width, jint height) {
  return lumacore_render_init(reinterpret_cast<void*>(surface), width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeRelease(JNIEnv*, jobject, jlong session) {
  lumacore_release(session);
}

// Converts Flutter's TextureRegistry-provided android.view.Surface into an
// ANativeWindow* usable by eglCreateWindowSurface — the jlong nativeRenderInit
// takes as `surface`. ANativeWindow_fromSurface itself takes the +1 ref that
// GLRenderBackend::teardown() releases; nothing else in this file owns it.
extern "C" JNIEXPORT jlong JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeAcquireWindow(JNIEnv* env, jobject, jobject surface) {
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  return reinterpret_cast<jlong>(window);
}

// Android push-model counterpart to iOS's CVPixelBufferRef import: Kotlin
// needs this GL_TEXTURE_EXTERNAL_OES name to construct the camera-input
// `SurfaceTexture(textureId)` (see EffectsRenderController.kt). -1 if
// session is invalid.
extern "C" JNIEXPORT jint JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeGetCameraTextureId(JNIEnv*, jobject, jlong session) {
  return lumacore_get_camera_texture_id(session);
}

extern "C" JNIEXPORT void JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeSetThermalState(JNIEnv*, jobject, jlong session, jint state) {
  lumacore_set_thermal_state(session, state);
}

// Refreshed every frame from SurfaceTexture.getTransformMatrix() right
// after updateTexImage(), before nativeRenderFrame — see
// ai_plans/04-android-camerax-gl-pipeline.md §C follow-up (sensor-orientation
// fix). Critical section: called every frame on the GL thread, no other JNI
// calls needed while held.
extern "C" JNIEXPORT void JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeSetCameraTransform(JNIEnv* env, jobject, jlong session,
                                                                  jfloatArray matrix) {
  jfloat* elems = env->GetFloatArrayElements(matrix, nullptr);
  lumacore_set_camera_transform(session, elems);
  env->ReleaseFloatArrayElements(matrix, elems, JNI_ABORT);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeStartRecording(JNIEnv* env, jobject, jlong session, jstring outPath,
                                                              jint bitrateKbps, jint w, jint h) {
  const char* path = env->GetStringUTFChars(outPath, nullptr);
  int32_t result = lumacore_start_recording(session, path, bitrateKbps, w, h);
  env->ReleaseStringUTFChars(outPath, path);
  return result;
}

// No cameraFrame/outPreviewImage unlike iOS's lumacore_render_frame call
// site — the camera frame is already bound into the GL external-OES texture
// via SurfaceTexture.updateTexImage() on the GL thread before this is
// called, and the rendered frame goes straight to the screen via
// eglSwapBuffers inside GLRenderBackend::endFrame(), not back through a CPU
// handle (see ai_plans/04 §A.3). The non-null sentinel below only exists to
// satisfy lumacore_render_frame's shared (platform-agnostic) `!cameraFrame`
// guard — GLRenderBackend::importExternalFrame() ignores its content.
extern "C" JNIEXPORT jint JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeRenderFrame(JNIEnv*, jobject, jlong session, jlong ptsUs) {
  void* ignoredSentinel = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
  return lumacore_render_frame(session, ignoredSentinel, ptsUs, nullptr);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeStopRecording(JNIEnv*, jobject, jlong session) {
  return lumacore_stop_recording(session);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeSubmitAudioFrame(JNIEnv* env, jobject, jlong session,
                                                                jbyteArray pcmData, jint numFrames, jint sampleRate,
                                                                jint numChannels, jlong ptsUs) {
  // Critical section, not GetByteArrayElements — this runs once per audio
  // buffer (~every 23ms) on AudioRecordController's own thread and never
  // calls back into JNI, so the usual "no other JNI calls while held"
  // restriction costs nothing here and avoids a copy.
  void* pcm = env->GetPrimitiveArrayCritical(pcmData, nullptr);
  int32_t result = lumacore_submit_audio_frame(session, pcm, numFrames, sampleRate, numChannels, ptsUs);
  env->ReleasePrimitiveArrayCritical(pcmData, pcm, JNI_ABORT);
  return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeValidateLicense(JNIEnv* env, jobject, jstring tokenBlobJson,
                                                               jstring deviceFingerprint) {
  const char* token = env->GetStringUTFChars(tokenBlobJson, nullptr);
  const char* fingerprint = env->GetStringUTFChars(deviceFingerprint, nullptr);
  int32_t result = lumacore_validate_license(token, fingerprint);
  env->ReleaseStringUTFChars(tokenBlobJson, token);
  env->ReleaseStringUTFChars(deviceFingerprint, fingerprint);
  return result;
}

#endif  // __ANDROID__
