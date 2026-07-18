#pragma once

#include <memory>

#include "EffectParams.h"
#include "IRenderBackend.h"

namespace lumacore::render {

// Shared orchestration: pass sequence, FBO ping-pong, frame timing. Owns a
// platform IRenderBackend and drives the same 3-pass pipeline (color
// correction -> vignette -> particles) on every platform. See ARCHITECTURE.md §2.
class RenderPipeline {
 public:
  explicit RenderPipeline(std::unique_ptr<IRenderBackend> backend);

  bool initialize(const RenderContextParams& params);
  TextureHandle renderFrame(NativeImageHandle cameraFrame);
  void setEffectParams(const LumaEffectParams& params);
  void shutdown();

 private:
  std::unique_ptr<IRenderBackend> backend_;
  EffectParamsBlock currentParams_{};
};

}  // namespace lumacore::render
