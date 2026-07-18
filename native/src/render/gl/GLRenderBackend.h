#pragma once

#include "render/IRenderBackend.h"

namespace lumacore::render::gl {

// Android backend: EGL context/surface, glEGLImageTargetTexture2DOES import,
// runtime GLSL compilation. See ARCHITECTURE.md §2. Stub — implemented in Этап 4.
class GLRenderBackend : public IRenderBackend {
 public:
  bool initialize(const RenderContextParams&) override;
  TextureHandle importExternalFrame(NativeImageHandle) override;
  void beginFrame() override;
  void runPass(PassId pass, TextureHandle src, TextureHandle dstFbo, const EffectParamsBlock& params) override;
  TextureHandle endFrame() override;
  PlatformImageHandle exportForPreview(TextureHandle) override;
  PlatformImageHandle exportForEncoder(TextureHandle) override;
  void destroy() override;
};

}  // namespace lumacore::render::gl
