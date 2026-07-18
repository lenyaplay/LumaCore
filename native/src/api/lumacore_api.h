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
} LumaEffectParams;

typedef struct {
  double fps, avgFrameMs;
  uint32_t droppedFrames;
  int32_t thermalState;
} LumaStats;

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

#ifdef __cplusplus
}
#endif
