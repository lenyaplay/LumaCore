#include "RenderPipeline.h"

namespace lumacore::render {

RenderPipeline::RenderPipeline(std::unique_ptr<IRenderBackend> backend) : backend_(std::move(backend)) {}

bool RenderPipeline::initialize(const RenderContextParams& params) {
  if (!backend_) return false;
  return backend_->initialize(params);
}

TextureHandle RenderPipeline::renderFrame(NativeImageHandle cameraFrame) {
  // TODO(Этап 4): import frame, run ColorCorrection -> Vignette -> Particles
  // passes through backend_->runPass(), then endFrame(). Particle math (pure
  // C++, shared across backends) plugs in here.
  if (!backend_) return nullptr;
  backend_->beginFrame();
  TextureHandle src = backend_->importExternalFrame(cameraFrame);
  (void)src;
  return backend_->endFrame();
}

void RenderPipeline::setEffectParams(const LumaEffectParams& params) {
  currentParams_ = toEffectParamsBlock(params);
}

void RenderPipeline::shutdown() {
  if (backend_) backend_->destroy();
}

}  // namespace lumacore::render
