#ifdef __ANDROID__

#include <jni.h>

#include "api/lumacore_api.h"

// Thin JNIEXPORT layer only — no logic here, everything forwards to
// lumacore_api.h. See ARCHITECTURE.md §5 (jni/ is a language-boundary shim,
// not a place for behavior). Stub — implemented alongside CameraX glue in Этап 2.

extern "C" JNIEXPORT jlong JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeRenderInit(JNIEnv*, jobject, jlong surface, jint width, jint height) {
  return lumacore_render_init(reinterpret_cast<void*>(surface), width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_lumacore_native_LumaCoreBridge_nativeRelease(JNIEnv*, jobject, jlong session) {
  lumacore_release(session);
}

#endif  // __ANDROID__
