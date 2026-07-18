#pragma once

#include "render/IRenderBackend.h"

namespace lumacore::render::vulkan {

// Windows backend: offscreen VkDevice/VkQueue, VMA-managed memory, explicit
// VkImageMemoryBarrier between passes, precompiled SPIR-V shaders. Frame
// import is D3D11-resident (IMFDXGIBuffer) when the driver allows it, else a
// CPU staging-buffer copy (base case). See ARCHITECTURE.md §2. Stub — Этап 10.
class VulkanRenderBackend : public IRenderBackend {
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

}  // namespace lumacore::render::vulkan
