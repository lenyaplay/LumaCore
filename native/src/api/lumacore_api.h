#pragma once

#include <stdint.h>

#ifdef _WIN32
#ifdef LUMACORE_EXPORTS
#define LUMACORE_API __declspec(dllexport)
#else
#define LUMACORE_API __declspec(dllimport)
#endif
#elif defined(__APPLE__)
// Required for DynamicLibrary.process() on iOS (ai_plans/03 §9): without
// this, the linker strips symbols referenced only from Dart via dart:ffi
// when statically linking lumacore into the app binary.
#define LUMACORE_API __attribute__((visibility("default"))) __attribute__((used))
#else
#define LUMACORE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float brightness, contrast, saturation, vignetteRadius, vignetteSoftness, particleIntensity;
  int64_t effectMask;
  float sepiaAmount, edgeThreshold, edgeIntensity;
} LumaEffectParams;

typedef struct {
  double fps, avgFrameMs;
  uint32_t droppedFrames;
  int32_t thermalState;
} LumaStats;

// Mirrors lumacore::license::LicenseStatus (native/src/license/TokenValidator.h).
typedef enum {
  LUMACORE_LICENSE_VALID = 0,
  LUMACORE_LICENSE_EXPIRED = 1,
  LUMACORE_LICENSE_DEVICE_MISMATCH = 2,
  LUMACORE_LICENSE_INVALID_SIGNATURE = 3,
  LUMACORE_LICENSE_NOT_ACTIVATED = 4,
} LumaLicenseStatus;

// Initializes the render pipeline (RenderPipeline + platform IRenderBackend)
// for this session. Returns -1 if the backend fails to initialize (Metal
// device/library/pool setup, etc.) — unlike the Этап 1-3 passthrough
// version, this can now genuinely fail.
LUMACORE_API int64_t lumacore_render_init(void* platformSurfaceOrCtx, int width, int height);
LUMACORE_API void lumacore_set_effect_params(int64_t session, const LumaEffectParams* params);
LUMACORE_API void lumacore_get_stats(int64_t session, LumaStats* outStats);
LUMACORE_API void lumacore_set_thermal_state(int64_t session, int32_t state);
LUMACORE_API int32_t lumacore_start_recording(int64_t session, const char* outPath, int bitrateKbps, int w, int h);
// Runs cameraFrame through the full 3-pass pipeline, hands back the preview
// frame via outPreviewImage (+1 retained CVPixelBufferRef on iOS — the
// caller must release it), and — if a recording is active — forwards the
// exportForEncoder() result into EncoderSession::submitFrame internally, all
// within one call. Replaces the Этап 1-3 lumacore_submit_frame passthrough:
// Swift now makes exactly one native call per camera frame, not two.
// Returns 0 on success, -1 if the frame was dropped (thermal/pool pressure)
// or the session is not ready.
LUMACORE_API int32_t lumacore_render_frame(int64_t session, void* cameraFrame, int64_t ptsUs,
                                            void** outPreviewImage);
LUMACORE_API int32_t lumacore_stop_recording(int64_t session);
LUMACORE_API void lumacore_release(int64_t session);

// Offline license verification (ARCHITECTURE.md §6) — not tied to a render
// session, no network access. Returns a LumaLicenseStatus value.
LUMACORE_API int32_t lumacore_validate_license(const char* tokenBlobJson, const char* deviceFingerprint);

// Submits a chunk of interleaved PCM audio to the active recording's AAC
// stream, on the same absolute clock as lumacore_render_frame's ptsUs (see
// EncoderSession — the two streams share one PTS origin). No-op if no
// recording is active. Returns 0 on success, -1 if the session/args are
// invalid.
LUMACORE_API int32_t lumacore_submit_audio_frame(int64_t session, const void* pcmData, int32_t numFrames,
                                                  int32_t sampleRate, int32_t numChannels, int64_t ptsUs);

#if defined(__ANDROID__)
// Returns the GL_TEXTURE_EXTERNAL_OES texture name the camera must write
// into via Kotlin's `SurfaceTexture(textureId)` — the Android push-model
// counterpart to iOS passing a CVPixelBufferRef into lumacore_render_frame
// directly (see ai_plans/04-android-camerax-gl-pipeline.md §A.3). -1 if
// session is invalid. Android-only: there is nothing to expose here on
// platforms whose backend doesn't own an externally-fed GL texture.
LUMACORE_API int32_t lumacore_get_camera_texture_id(int64_t session);

// Forwards SurfaceTexture.getTransformMatrix()'s 4x4 column-major matrix
// (exactly 16 floats) — must be refreshed every frame, right after
// updateTexImage(), before the corresponding lumacore_render_frame call.
// Without it, the color-correction shader samples the sensor's native
// buffer layout, which is rotated/mirrored relative to what should be
// displayed. No-op if session is invalid.
LUMACORE_API void lumacore_set_camera_transform(int64_t session, const float* matrix16);
#endif

#ifdef __cplusplus
}
#endif
