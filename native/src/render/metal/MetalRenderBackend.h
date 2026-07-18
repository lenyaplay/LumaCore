#pragma once

#include <memory>

#include "render/IRenderBackend.h"

namespace lumacore::render::metal {

// iOS backend: MTLDevice/CAMetalLayer, CVMetalTextureCache zero-copy import,
// precompiled .metallib. See ARCHITECTURE.md §2. Objective-C++ (.mm) because
// Metal has no C API.
//
// Pimpl'd so this header stays pure C++ (no ObjC syntax in signatures) —
// PlatformRenderBackendFactory.cpp is a plain .cpp (compiled as C++, not
// Objective-C++, regardless of platform) and must be able to name this type
// to instantiate it. All Metal/CoreVideo types live in Impl, defined only in
// MetalRenderBackend.mm.
class MetalRenderBackend : public IRenderBackend {
 public:
  MetalRenderBackend();
  ~MetalRenderBackend() override;

  bool initialize(const RenderContextParams&) override;
  TextureHandle importExternalFrame(NativeImageHandle) override;
  void beginFrame() override;
  void runPass(PassId pass, TextureHandle src, TextureHandle dstFbo, const EffectParamsBlock& params) override;
  TextureHandle endFrame() override;
  PlatformImageHandle exportForPreview(TextureHandle) override;
  PlatformImageHandle exportForEncoder(TextureHandle) override;
  void destroy() override;

  // Public only so MetalRenderBackend.mm's Impl-scoped helper functions can
  // name it — the definition lives entirely in the .mm (ObjC-type-free header).
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace lumacore::render::metal
