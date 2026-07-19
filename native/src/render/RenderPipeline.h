#pragma once

#include <cstdint>
#include <memory>

#include "EffectParams.h"
#include "IRenderBackend.h"
#include "api/lumacore_api.h"

namespace lumacore::render {

// Shared orchestration: pass sequence, thermal-aware effect gating, frame
// timing. Owns a platform IRenderBackend and drives the same 3-pass pipeline
// (color correction -> vignette -> particles) on every platform. See
// ARCHITECTURE.md §2, ai_plans/03-ios-metal-render-pipeline.md §5.
class RenderPipeline {
 public:
  explicit RenderPipeline(std::unique_ptr<IRenderBackend> backend);

  bool initialize(const RenderContextParams& params);
  // false = frame dropped (pool exhausted / backend not ready).
  bool renderFrame(NativeImageHandle cameraFrame, double elapsedSeconds);
  PlatformImageHandle exportForPreview();
  PlatformImageHandle exportForEncoder();
  void setEffectParams(const LumaEffectParams& params);
  void setThermalState(int32_t state);  // 0..3, mirrors ProcessInfo.ThermalState
  LumaStats getStats() const;
  void shutdown();

  // Escape hatch for platform-specific queries that don't belong on the
  // shared IRenderBackend interface (e.g. Android's GLRenderBackend::
  // cameraTextureId() — see lumacore_get_camera_texture_id in
  // lumacore_api.cpp). Callers must know and static_cast to the concrete
  // backend type themselves; RenderPipeline stays backend-agnostic.
  IRenderBackend* rawBackend() const { return backend_.get(); }

 private:
  std::unique_ptr<IRenderBackend> backend_;
  EffectParamsBlock currentParams_{};
  TextureHandle lastFrame_ = nullptr;
  int32_t thermalState_ = 0;

  // Sliding window of the last N renderFrame() timestamps — NOT
  // util::RingBuffer<T> (that's a mutex-locking producer/consumer queue with
  // destructive pop and no non-destructive iteration for averaging; wrong
  // tool here). renderFrame() is only ever called from one thread
  // (ARCHITECTURE.md §1 — MetalRenderThread ticks on camera frame arrival,
  // one thread for the whole path), so no locking is needed at all — a plain
  // fixed-size circular array local to this class.
  static constexpr int kStatsWindow = 32;
  double frameTimestamps_[kStatsWindow] = {};
  int frameTimestampCount_ = 0;
  int frameTimestampHead_ = 0;
  uint32_t droppedFrames_ = 0;
};

}  // namespace lumacore::render
