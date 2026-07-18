#include "VulkanRenderBackend.h"

namespace lumacore::render::vulkan {

bool VulkanRenderBackend::initialize(const RenderContextParams&) { return false; }

TextureHandle VulkanRenderBackend::importExternalFrame(NativeImageHandle) { return nullptr; }

void VulkanRenderBackend::beginFrame() {}

void VulkanRenderBackend::runPass(PassId, TextureHandle, TextureHandle, const EffectParamsBlock&) {}

TextureHandle VulkanRenderBackend::endFrame() { return nullptr; }

PlatformImageHandle VulkanRenderBackend::exportForPreview(TextureHandle) { return nullptr; }

PlatformImageHandle VulkanRenderBackend::exportForEncoder(TextureHandle) { return nullptr; }

void VulkanRenderBackend::destroy() {}

}  // namespace lumacore::render::vulkan
