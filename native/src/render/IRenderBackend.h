#pragma once

#include "EffectParams.h"

namespace lumacore::render {

using TextureHandle = void*;
using NativeImageHandle = void*;
using PlatformImageHandle = void*;

enum class PassId { ColorCorrection, Vignette, Particles };

struct RenderContextParams {
  void* platformSurfaceOrCtx;
  int width;
  int height;
};

// Platform seam of the render pipeline (ARCHITECTURE.md §2). One implementation
// per platform: GLRenderBackend (Android), MetalRenderBackend (iOS),
// VulkanRenderBackend (Windows). RenderPipeline owns one of these and drives it.
class IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;

  virtual bool initialize(const RenderContextParams&) = 0;
  virtual TextureHandle importExternalFrame(NativeImageHandle) = 0;
  virtual void beginFrame() = 0;
  virtual void runPass(PassId pass, TextureHandle src, TextureHandle dstFbo,
                        const EffectParamsBlock& params) = 0;
  virtual TextureHandle endFrame() = 0;
  virtual PlatformImageHandle exportForPreview(TextureHandle) = 0;
  virtual PlatformImageHandle exportForEncoder(TextureHandle) = 0;
  virtual void destroy() = 0;
};

}  // namespace lumacore::render
