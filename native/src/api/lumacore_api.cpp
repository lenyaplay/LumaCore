#include "lumacore_api.h"

#include <unordered_map>

#include "encode/EncoderSession.h"
#include "render/RenderPipeline.h"

namespace {

// TODO(Этап 2-5): replace with a real session registry once a platform
// IRenderBackend and EncoderSession are wired together per-platform.
struct Session {
  lumacore::encode::EncoderSession encoder;
};

std::unordered_map<int64_t, Session> g_sessions;
int64_t g_nextSessionId = 1;

}  // namespace

int64_t lumacore_render_init(void* /*platformSurfaceOrCtx*/, int /*width*/, int /*height*/) {
  int64_t id = g_nextSessionId++;
  g_sessions.emplace(id, Session{});
  return id;
}

void lumacore_set_effect_params(int64_t /*session*/, const LumaEffectParams* /*params*/) {
  // TODO: forward to the session's RenderPipeline::setEffectParams.
}

void lumacore_get_stats(int64_t /*session*/, LumaStats* outStats) {
  if (!outStats) return;
  *outStats = LumaStats{};
}

int32_t lumacore_start_recording(int64_t session, const char* outPath, int bitrateKbps, int w, int h) {
  auto it = g_sessions.find(session);
  if (it == g_sessions.end() || !outPath) return -1;
  return it->second.encoder.start(outPath, bitrateKbps, w, h) ? 0 : -1;
}

int32_t lumacore_stop_recording(int64_t session) {
  auto it = g_sessions.find(session);
  if (it == g_sessions.end()) return -1;
  return it->second.encoder.stop() ? 0 : -1;
}

void lumacore_release(int64_t session) { g_sessions.erase(session); }
