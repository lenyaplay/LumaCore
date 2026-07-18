#include "RenderPipeline.h"

namespace lumacore::render {

namespace {
// Mirrors ProcessInfo.ThermalState: 0=nominal, 1=fair, 2=serious, 3=critical.
constexpr int32_t kThermalThrottleState = 2;
// Must stay in sync with LUMACORE_EFFECT_PARTICLES in
// render/metal/shaders/ShaderTypes.h (not included here — that header pulls
// in <simd/simd.h> in its non-Metal branch, which only exists on Apple
// platforms, and RenderPipeline.cpp also builds on the plain host skeleton).
constexpr int64_t kParticlesEffectBit = 0x4;
}  // namespace

RenderPipeline::RenderPipeline(std::unique_ptr<IRenderBackend> backend) : backend_(std::move(backend)) {}

bool RenderPipeline::initialize(const RenderContextParams& params) {
  if (!backend_) return false;
  return backend_->initialize(params);
}

bool RenderPipeline::renderFrame(NativeImageHandle cameraFrame, double elapsedSeconds) {
  if (!backend_) return false;

  backend_->beginFrame();
  TextureHandle src = backend_->importExternalFrame(cameraFrame);
  if (!src) {
    ++droppedFrames_;
    return false;
  }

  backend_->runPass(PassId::ColorCorrection, src, nullptr, currentParams_);
  backend_->runPass(PassId::Vignette, nullptr, nullptr, currentParams_);

  // Thermal ladder, rung 1: force particles off under throttling. The pass
  // still runs — it also performs the RGBA->NV12 conversion — just without
  // the particle draw call.
  EffectParamsBlock particleParams = currentParams_;
  if (thermalState_ >= kThermalThrottleState) {
    particleParams.effectMask &= ~kParticlesEffectBit;
  }
  backend_->runPass(PassId::Particles, nullptr, nullptr, particleParams);

  lastFrame_ = backend_->endFrame();
  if (!lastFrame_) {
    ++droppedFrames_;
    return false;
  }

  frameTimestamps_[frameTimestampHead_] = elapsedSeconds;
  frameTimestampHead_ = (frameTimestampHead_ + 1) % kStatsWindow;
  if (frameTimestampCount_ < kStatsWindow) ++frameTimestampCount_;

  return true;
}

PlatformImageHandle RenderPipeline::exportForPreview() {
  if (!backend_ || !lastFrame_) return nullptr;
  return backend_->exportForPreview(lastFrame_);
}

PlatformImageHandle RenderPipeline::exportForEncoder() {
  if (!backend_ || !lastFrame_) return nullptr;
  return backend_->exportForEncoder(lastFrame_);
}

void RenderPipeline::setEffectParams(const LumaEffectParams& params) {
  currentParams_ = toEffectParamsBlock(params);
}

void RenderPipeline::setThermalState(int32_t state) { thermalState_ = state; }

LumaStats RenderPipeline::getStats() const {
  LumaStats stats{};
  stats.droppedFrames = droppedFrames_;
  stats.thermalState = thermalState_;

  if (frameTimestampCount_ < 2) return stats;

  // frameTimestampHead_ points one past the most recently written entry.
  int newestIdx = (frameTimestampHead_ - 1 + kStatsWindow) % kStatsWindow;
  int oldestIdx = (frameTimestampHead_ - frameTimestampCount_ + kStatsWindow) % kStatsWindow;
  double newest = frameTimestamps_[newestIdx];
  double oldest = frameTimestamps_[oldestIdx];
  double span = newest - oldest;
  int intervals = frameTimestampCount_ - 1;
  if (span > 0.0 && intervals > 0) {
    double avgIntervalSec = span / intervals;
    stats.avgFrameMs = avgIntervalSec * 1000.0;
    stats.fps = 1.0 / avgIntervalSec;
  }
  return stats;
}

void RenderPipeline::shutdown() {
  if (backend_) backend_->destroy();
  lastFrame_ = nullptr;
}

}  // namespace lumacore::render
