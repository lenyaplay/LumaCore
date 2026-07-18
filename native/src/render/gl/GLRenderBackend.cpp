#include "GLRenderBackend.h"

namespace lumacore::render::gl {

bool GLRenderBackend::initialize(const RenderContextParams&) { return false; }

TextureHandle GLRenderBackend::importExternalFrame(NativeImageHandle) { return nullptr; }

void GLRenderBackend::beginFrame() {}

void GLRenderBackend::runPass(PassId, TextureHandle, TextureHandle, const EffectParamsBlock&) {}

TextureHandle GLRenderBackend::endFrame() { return nullptr; }

PlatformImageHandle GLRenderBackend::exportForPreview(TextureHandle) { return nullptr; }

PlatformImageHandle GLRenderBackend::exportForEncoder(TextureHandle) { return nullptr; }

void GLRenderBackend::destroy() {}

}  // namespace lumacore::render::gl
