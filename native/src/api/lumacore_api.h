#pragma once

#include <stdint.h>

#ifdef _WIN32
#ifdef LUMACORE_EXPORTS
#define LUMACORE_API __declspec(dllexport)
#else
#define LUMACORE_API __declspec(dllimport)
#endif
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

LUMACORE_API int64_t lumacore_render_init(void* platformSurfaceOrCtx, int width, int height);
LUMACORE_API void lumacore_set_effect_params(int64_t session, const LumaEffectParams* params);
LUMACORE_API void lumacore_get_stats(int64_t session, LumaStats* outStats);
LUMACORE_API int32_t lumacore_start_recording(int64_t session, const char* outPath, int bitrateKbps, int w, int h);
LUMACORE_API int32_t lumacore_stop_recording(int64_t session);
LUMACORE_API void lumacore_release(int64_t session);

#ifdef __cplusplus
}
#endif
