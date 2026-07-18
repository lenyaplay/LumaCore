#pragma once

#include "render/IRenderBackend.h"

namespace lumacore::render::metal {

// iOS backend: MTLDevice/CAMetalLayer, CVMetalTextureCache zero-copy import,
// precompiled .metallib. See ARCHITECTURE.md §2. Stub — implemented in Этап 4.
// Objective-C++ (.mm) because Metal has no C API.
class MetalRenderBackend : public IRenderBackend {
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

}  // namespace lumacore::render::metal
