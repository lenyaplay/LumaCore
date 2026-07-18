#include "lumacore_api.h"

#include <chrono>
#include <unordered_map>

#include "encode/EncoderSession.h"
#include "license/TokenValidator.h"
#include "render/PlatformRenderBackendFactory.h"
#include "render/RenderPipeline.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <CoreVideo/CoreVideo.h>
#endif

namespace {

struct Session {
  lumacore::render::RenderPipeline pipeline{lumacore::render::createPlatformRenderBackend()};
  lumacore::encode::EncoderSession encoder;
  double startTime = 0;  // monotonic, elapsedSeconds fed into RenderPipeline::renderFrame for stats
  bool recording = false;
};

std::unordered_map<int64_t, Session> g_sessions;
int64_t g_nextSessionId = 1;

double monotonicSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// exportForEncoder() hands back a +1 owned platform image (CVPixelBufferRef
// on iOS); EncoderSession::submitFrame only reads it, so the caller here
// must release it. void* on non-Apple platforms carries nothing to release.
void releaseEncoderExport(void* handle) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  if (handle) CVPixelBufferRelease(static_cast<CVPixelBufferRef>(handle));
#else
  (void)handle;
#endif
}

}  // namespace

int64_t lumacore_render_init(void* platformSurfaceOrCtx, int width, int height) {
  int64_t id = g_nextSessionId++;
  Session& session = g_sessions.emplace(id, Session{}).first->second;

  lumacore::render::RenderContextParams params{platformSurfaceOrCtx, width, height};
  if (!session.pipeline.initialize(params)) {
    g_sessions.erase(id);
    return -1;
  }
  session.startTime = monotonicSeconds();
  return id;
}

void lumacore_set_effect_params(int64_t session, const LumaEffectParams* params) {
  auto it = g_sessions.find(session);
  if (it == g_sessions.end() || !params) return;
  it->second.pipeline.setEffectParams(*params);
}

void lumacore_get_stats(int64_t session, LumaStats* outStats) {
  if (!outStats) return;
  auto it = g_sessions.find(session);
  if (it == g_sessions.end()) {
    *outStats = LumaStats{};
    return;
  }
  *outStats = it->second.pipeline.getStats();
}

void lumacore_set_thermal_state(int64_t session, int32_t state) {
  auto it = g_sessions.find(session);
  if (it == g_sessions.end()) return;
  it->second.pipeline.setThermalState(state);
}

int32_t lumacore_start_recording(int64_t session, const char* outPath, int bitrateKbps, int w, int h) {
  auto it = g_sessions.find(session);
  if (it == g_sessions.end() || !outPath) return -1;
  bool started = it->second.encoder.start(outPath, bitrateKbps, w, h);
  it->second.recording = started;
  return started ? 0 : -1;
}

int32_t lumacore_render_frame(int64_t session, void* cameraFrame, int64_t ptsUs, void** outPreviewImage) {
  if (outPreviewImage) *outPreviewImage = nullptr;
  auto it = g_sessions.find(session);
  if (it == g_sessions.end() || !cameraFrame) return -1;

  Session& s = it->second;
  double elapsedSeconds = monotonicSeconds() - s.startTime;
  if (!s.pipeline.renderFrame(cameraFrame, elapsedSeconds)) return -1;

  if (outPreviewImage) {
    *outPreviewImage = s.pipeline.exportForPreview();
  }

  if (s.recording) {
    void* encoderImage = s.pipeline.exportForEncoder();
    if (encoderImage) {
      s.encoder.submitFrame(encoderImage, ptsUs);
      releaseEncoderExport(encoderImage);
    }
  }

  return 0;
}

int32_t lumacore_stop_recording(int64_t session) {
  auto it = g_sessions.find(session);
  if (it == g_sessions.end()) return -1;
  it->second.recording = false;
  return it->second.encoder.stop() ? 0 : -1;
}

void lumacore_release(int64_t session) {
  auto it = g_sessions.find(session);
  if (it == g_sessions.end()) return;
  it->second.pipeline.shutdown();
  g_sessions.erase(it);
}

int32_t lumacore_submit_audio_frame(int64_t session, const void* pcmData, int32_t numFrames, int32_t sampleRate,
                                     int32_t numChannels, int64_t ptsUs) {
  auto it = g_sessions.find(session);
  if (it == g_sessions.end() || !pcmData || numFrames <= 0) return -1;
  Session& s = it->second;
  if (!s.recording) return -1;  // same api-layer gate lumacore_render_frame uses for video
  s.encoder.submitAudioFrame(pcmData, numFrames, sampleRate, numChannels, ptsUs);
  return 0;
}

int32_t lumacore_validate_license(const char* tokenBlobJson, const char* deviceFingerprint) {
  if (!tokenBlobJson || !deviceFingerprint) {
    return static_cast<int32_t>(lumacore::license::LicenseStatus::NotActivated);
  }
  // Fetched once from the locally-running mock server's GET /public-key
  // (server/app/crypto.py, seed persisted to server/.dev_signing_key so this
  // stays valid across server restarts — see server/app/crypto.py). Only the
  // public key is embedded, per ARCHITECTURE.md §6.
  static const lumacore::license::TokenValidator kValidator("PqfyQCNi4Pv5t3fP7b/XSfUIzkw70DaUaTLDGm2JuRg=");
  return static_cast<int32_t>(kValidator.validate(tokenBlobJson, deviceFingerprint));
}
