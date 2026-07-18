#import "MetalRenderBackend.h"

namespace lumacore::render::metal {

bool MetalRenderBackend::initialize(const RenderContextParams&) { return false; }

TextureHandle MetalRenderBackend::importExternalFrame(NativeImageHandle) { return nullptr; }

void MetalRenderBackend::beginFrame() {}

void MetalRenderBackend::runPass(PassId, TextureHandle, TextureHandle, const EffectParamsBlock&) {}

TextureHandle MetalRenderBackend::endFrame() { return nullptr; }

PlatformImageHandle MetalRenderBackend::exportForPreview(TextureHandle) { return nullptr; }

PlatformImageHandle MetalRenderBackend::exportForEncoder(TextureHandle) { return nullptr; }

void MetalRenderBackend::destroy() {}

}  // namespace lumacore::render::metal
