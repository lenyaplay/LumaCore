#pragma once

#include <memory>

#include "render/IRenderBackend.h"

namespace lumacore::render::gl {

// Android backend: EGL context/surface, GL_TEXTURE_EXTERNAL_OES camera
// import, runtime-compiled GLSL ES 3.0. See ARCHITECTURE.md §2,
// ai_plans/04-android-camerax-gl-pipeline.md §B.
//
// Push model, not pull like Metal: the caller (GLRenderThread on the Kotlin
// side) already bound the camera's latest frame into the external OES
// texture via SurfaceTexture.updateTexImage() *before* calling
// importExternalFrame() — the NativeImageHandle argument is therefore
// ignored (documented on IRenderBackend as the one platform-specific
// asymmetry in that contract, see the plan §A.3). endFrame() also differs
// from Metal: presentation is a direct blit into the ANativeWindow-bound
// EGLSurface + eglSwapBuffers, not a CPU pixel-buffer handoff.
//
// Pimpl'd for the same reason as MetalRenderBackend: keeps EGL/GLES types
// out of this header so PlatformRenderBackendFactory.cpp (compiled on every
// platform) can name this class without pulling in <GLES3/gl3.h>.
class GLRenderBackend : public IRenderBackend {
 public:
  GLRenderBackend();
  ~GLRenderBackend() override;

  bool initialize(const RenderContextParams&) override;
  TextureHandle importExternalFrame(NativeImageHandle) override;
  void beginFrame() override;
  void runPass(PassId pass, TextureHandle src, TextureHandle dstFbo, const EffectParamsBlock& params) override;
  TextureHandle endFrame() override;
  PlatformImageHandle exportForPreview(TextureHandle) override;
  PlatformImageHandle exportForEncoder(TextureHandle) override;
  void destroy() override;

  // Not part of IRenderBackend — Kotlin needs the raw external-OES texture
  // name to construct its camera-input `SurfaceTexture(textureId)` (see
  // JNI nativeGetCameraTextureId). 0 before initialize() succeeds.
  unsigned int cameraTextureId() const;

  // SurfaceTexture.getTransformMatrix()'s 4x4 (column-major, GL-uniform-
  // ready) matrix, refreshed by the caller every frame right after
  // updateTexImage() — without this, sampling uCamera with the raw fullscreen
  // UV shows the sensor's native buffer layout, which is rotated/mirrored
  // relative to what the app should display (see JNI
  // nativeSetCameraTransform, ai_plans/04 §C follow-up). Not part of
  // IRenderBackend for the same reason as cameraTextureId().
  void setCameraTransform(const float matrix[16]);

  // Public only so GLRenderBackend.cpp's Impl-scoped helper functions can
  // name it — the definition lives entirely in the .cpp (EGL/GLES-type-free
  // header).
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace lumacore::render::gl
