#include "GLRenderBackend.h"

// Order matters: gl2ext.h's typedefs reference GLenum/GLuint/etc., which it
// expects a GLES header to have already defined — gl3.h must come first.
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>  // GL_TEXTURE_EXTERNAL_OES
#include <EGL/egl.h>
#include <android/log.h>
#include <android/native_window.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "encode/EncoderSession.h"
#include "render/particles/ParticleSystem.h"
#include "shaders/ShaderSources.h"

#define LUMACORE_LOG_TAG "LumaCoreGL"
#define LUMACORE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LUMACORE_LOG_TAG, __VA_ARGS__)

namespace lumacore::render::gl {

namespace {

// effectMask bits — must stay in sync by hand with ShaderTypes.h's
// LUMACORE_EFFECT_* defines (Metal side) and shaders/ShaderSources.h's
// hard-coded 0x1/0x2/0x8/0x10 literals (GLSL ES 3.0 has no #include of a
// shared header across the C++/GLSL boundary here). Not including
// ShaderTypes.h directly: its non-Metal branch pulls in <simd/simd.h>,
// Apple-only.
constexpr int32_t kEffectParticles = 0x4;

// GPU-side mirror of EffectParamsBlock, GLSL std140-compatible. Same
// hand-synced-across-files pattern as MetalRenderBackend.mm's
// EffectParamsGPU/toGPU — the one difference is effectMask truncated from
// int64_t to int32_t (see ai_plans/04-android-camerax-gl-pipeline.md context
// section: GLSL ES 3.0 has no 64-bit integer, all five current effect bits
// fit in 32 already).
struct alignas(16) EffectParamsGPU {
  float brightness, contrast, saturation, _pad0;
  float vignetteRadius, vignetteSoftness, particleIntensity, _pad1;
  int32_t effectMask, _pad2a, _pad2b, _pad2c;
  float sepiaAmount, edgeThreshold, edgeIntensity, _pad3;
};
static_assert(sizeof(EffectParamsGPU) == 64, "must match the std140 layout declared in ShaderSources.h");

EffectParamsGPU toGPU(const EffectParamsBlock& p) {
  EffectParamsGPU g{};
  g.brightness = p.brightness;
  g.contrast = p.contrast;
  g.saturation = p.saturation;
  g.vignetteRadius = p.vignetteRadius;
  g.vignetteSoftness = p.vignetteSoftness;
  g.particleIntensity = p.particleIntensity;
  g.effectMask = static_cast<int32_t>(p.effectMask);
  g.sepiaAmount = p.sepiaAmount;
  g.edgeThreshold = p.edgeThreshold;
  g.edgeIntensity = p.edgeIntensity;
  return g;
}

double monotonicSeconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

}  // namespace

// Private detail — does not change the public TextureHandle=void* contract
// of IRenderBackend. `texture` is meaningless for the camera-import handle
// (ColorCorrection always samples the external OES texture directly, never
// this field) — only the composite handle returned by endFrame() carries a
// real texture name, consumed by exportForEncoder().
struct GLTextureHandle {
  GLuint texture = 0;
};

struct GLRenderBackend::Impl {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface windowSurface = EGL_NO_SURFACE;
  ANativeWindow* nativeWindow = nullptr;  // +1 ref, acquired by the JNI caller, released in teardown()

  GLuint cameraTexture = 0;  // GL_TEXTURE_EXTERNAL_OES — camera writes here via SurfaceTexture.updateTexImage()

  GLuint colorCorrectProgram = 0;
  GLuint vignetteProgram = 0;
  GLuint blitProgram = 0;
  GLuint particleProgram = 0;
  GLuint nv12YProgram = 0;
  GLuint nv12CbCrProgram = 0;

  GLuint effectParamsUbo = 0;
  GLuint particleInstanceVbo = 0;

  GLuint colorCorrectFbo = 0, colorCorrectTex = 0;
  GLuint vignetteFbo = 0, vignetteTex = 0;
  GLuint particleCompositeFbo = 0, particleCompositeTex = 0;
  GLuint nv12YFbo = 0, nv12YTex = 0;
  GLuint nv12CbCrFbo = 0, nv12CbCrTex = 0;

  int width = 0;
  int height = 0;

  std::unique_ptr<lumacore::render::particles::ParticleSystem> particles;
  std::vector<lumacore::render::particles::ParticleInstance> instanceScratch;
  double startTime = 0.0;

  bool frameValid = false;
  GLTextureHandle compositeHandle;

  // Identity until the first setCameraTransform() call — SurfaceTexture.
  // getTransformMatrix() is column-major, same layout glUniformMatrix4fv
  // expects, so this is a straight memcpy target.
  float cameraTransform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  bool initialized = false;

  ~Impl() { teardown(); }

  static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
      GLint logLen = 0;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
      std::vector<char> log(static_cast<size_t>(logLen) + 1, 0);
      glGetShaderInfoLog(shader, logLen, nullptr, log.data());
      LUMACORE_LOGE("shader compile failed: %s", log.data());
      glDeleteShader(shader);
      return 0;
    }
    return shader;
  }

  static GLuint linkProgram(const char* vsSource, const char* fsSource) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSource);
    if (!vs || !fs) return 0;

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
      GLint logLen = 0;
      glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
      std::vector<char> log(static_cast<size_t>(logLen) + 1, 0);
      glGetProgramInfoLog(program, logLen, nullptr, log.data());
      LUMACORE_LOGE("program link failed: %s", log.data());
      glDeleteProgram(program);
      return 0;
    }
    return program;
  }

  // GLSL ES 3.0 has no `layout(std140, binding=N)` qualifier (that needs ES
  // 3.1) — the binding point is wired up from the host side instead.
  static bool bindEffectParamsBlock(GLuint program) {
    GLuint index = glGetUniformBlockIndex(program, "EffectParams");
    if (index == GL_INVALID_INDEX) return false;
    glUniformBlockBinding(program, index, /*binding=*/0);
    return true;
  }

  static GLuint makeTexture(GLenum internalFormat, GLenum format, GLenum type, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), w, h, 0, format, type, nullptr);
    return tex;
  }

  static GLuint makeFbo(GLuint texture) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      LUMACORE_LOGE("framebuffer incomplete: 0x%x", status);
      glDeleteFramebuffers(1, &fbo);
      return 0;
    }
    return fbo;
  }

  static void drawFullscreenTriangle() { glDrawArrays(GL_TRIANGLES, 0, 3); }

  void teardown() {
    if (display == EGL_NO_DISPLAY) return;  // never initialized, or already torn down

    // Deleting GL objects requires the context to still be current — it is,
    // since this runs on the same dedicated GL thread that created them
    // (see ai_plans/04 §B.2).
    GLuint textures[] = {cameraTexture,         colorCorrectTex, vignetteTex,
                          particleCompositeTex, nv12YTex,        nv12CbCrTex};
    glDeleteTextures(static_cast<GLsizei>(sizeof(textures) / sizeof(textures[0])), textures);
    GLuint fbos[] = {colorCorrectFbo, vignetteFbo, particleCompositeFbo, nv12YFbo, nv12CbCrFbo};
    glDeleteFramebuffers(static_cast<GLsizei>(sizeof(fbos) / sizeof(fbos[0])), fbos);
    GLuint programs[] = {colorCorrectProgram, vignetteProgram, blitProgram,
                          particleProgram,     nv12YProgram,    nv12CbCrProgram};
    for (GLuint p : programs) {
      if (p) glDeleteProgram(p);
    }
    if (effectParamsUbo) glDeleteBuffers(1, &effectParamsUbo);
    if (particleInstanceVbo) glDeleteBuffers(1, &particleInstanceVbo);

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (windowSurface != EGL_NO_SURFACE) eglDestroySurface(display, windowSurface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    eglTerminate(display);

    if (nativeWindow) {
      ANativeWindow_release(nativeWindow);
      nativeWindow = nullptr;
    }

    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    windowSurface = EGL_NO_SURFACE;
    initialized = false;
  }
};

GLRenderBackend::GLRenderBackend() : impl_(std::make_unique<Impl>()) {}

GLRenderBackend::~GLRenderBackend() { destroy(); }

bool GLRenderBackend::initialize(const RenderContextParams& params) {
  Impl& impl = *impl_;
  if (params.width <= 0 || params.height <= 0 || !params.platformSurfaceOrCtx) return false;

  impl.width = params.width;
  impl.height = params.height;
  // Acquired by the JNI layer's nativeAcquireWindow (ANativeWindow_fromSurface
  // already took the +1 ref) — we take ownership of releasing it in teardown().
  impl.nativeWindow = static_cast<ANativeWindow*>(params.platformSurfaceOrCtx);

  impl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (impl.display == EGL_NO_DISPLAY || !eglInitialize(impl.display, nullptr, nullptr)) return false;

  // EGL_OPENGL_ES3_BIT is core since EGL 1.5 (no eglext.h/_KHR suffix
  // needed) — all NDK versions in this project's dev environment (25.1+)
  // ship EGL 1.5 headers.
  const EGLint configAttribs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE,        8,                  EGL_GREEN_SIZE,   8,
      EGL_BLUE_SIZE,       8,                  EGL_ALPHA_SIZE,   8,
      EGL_NONE,
  };
  EGLConfig config;
  EGLint numConfigs = 0;
  if (!eglChooseConfig(impl.display, configAttribs, &config, 1, &numConfigs) || numConfigs == 0) return false;

  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  // One context for this backend's entire lifetime, created and used only on
  // the caller's dedicated GL thread (ai_plans/04 §B.2) — RenderPipeline/
  // IRenderBackend enforce no thread discipline themselves.
  impl.context = eglCreateContext(impl.display, config, EGL_NO_CONTEXT, contextAttribs);
  if (impl.context == EGL_NO_CONTEXT) return false;

  impl.windowSurface = eglCreateWindowSurface(impl.display, config, impl.nativeWindow, nullptr);
  if (impl.windowSurface == EGL_NO_SURFACE) {
    LUMACORE_LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
    return false;
  }

  if (!eglMakeCurrent(impl.display, impl.windowSurface, impl.windowSurface, impl.context)) {
    LUMACORE_LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
    return false;
  }

  impl.colorCorrectProgram =
      Impl::linkProgram(shaders::kFullscreenVertexSource, shaders::kColorCorrectFragmentSource);
  impl.vignetteProgram = Impl::linkProgram(shaders::kFullscreenVertexSource, shaders::kVignetteFragmentSource);
  impl.blitProgram = Impl::linkProgram(shaders::kFullscreenVertexSource, shaders::kBlitFragmentSource);
  impl.particleProgram = Impl::linkProgram(shaders::kParticleVertexSource, shaders::kParticleFragmentSource);
  impl.nv12YProgram = Impl::linkProgram(shaders::kFullscreenVertexSource, shaders::kNv12YFragmentSource);
  impl.nv12CbCrProgram = Impl::linkProgram(shaders::kFullscreenVertexSource, shaders::kNv12CbCrFragmentSource);
  if (!impl.colorCorrectProgram || !impl.vignetteProgram || !impl.blitProgram || !impl.particleProgram ||
      !impl.nv12YProgram || !impl.nv12CbCrProgram) {
    return false;
  }
  // blitProgram/particleProgram/nv12*Program don't declare the EffectParams
  // block — only these two read effect toggles/params.
  if (!Impl::bindEffectParamsBlock(impl.colorCorrectProgram) || !Impl::bindEffectParamsBlock(impl.vignetteProgram)) {
    return false;
  }

  glGenBuffers(1, &impl.effectParamsUbo);
  glBindBuffer(GL_UNIFORM_BUFFER, impl.effectParamsUbo);
  glBufferData(GL_UNIFORM_BUFFER, sizeof(EffectParamsGPU), nullptr, GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_UNIFORM_BUFFER, 0, impl.effectParamsUbo);

  glGenBuffers(1, &impl.particleInstanceVbo);

  glGenTextures(1, &impl.cameraTexture);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, impl.cameraTexture);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  impl.colorCorrectTex = Impl::makeTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, impl.width, impl.height);
  impl.vignetteTex = Impl::makeTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, impl.width, impl.height);
  impl.particleCompositeTex = Impl::makeTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, impl.width, impl.height);
  impl.nv12YTex = Impl::makeTexture(GL_R8, GL_RED, GL_UNSIGNED_BYTE, impl.width, impl.height);
  impl.nv12CbCrTex = Impl::makeTexture(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, impl.width / 2, impl.height / 2);
  if (!impl.colorCorrectTex || !impl.vignetteTex || !impl.particleCompositeTex || !impl.nv12YTex ||
      !impl.nv12CbCrTex) {
    return false;
  }

  impl.colorCorrectFbo = Impl::makeFbo(impl.colorCorrectTex);
  impl.vignetteFbo = Impl::makeFbo(impl.vignetteTex);
  impl.particleCompositeFbo = Impl::makeFbo(impl.particleCompositeTex);
  impl.nv12YFbo = Impl::makeFbo(impl.nv12YTex);
  impl.nv12CbCrFbo = Impl::makeFbo(impl.nv12CbCrTex);
  if (!impl.colorCorrectFbo || !impl.vignetteFbo || !impl.particleCompositeFbo || !impl.nv12YFbo ||
      !impl.nv12CbCrFbo) {
    return false;
  }

  impl.particles = std::make_unique<lumacore::render::particles::ParticleSystem>(200, /*seed=*/42);
  impl.startTime = monotonicSeconds();
  impl.initialized = true;
  return true;
}

unsigned int GLRenderBackend::cameraTextureId() const { return impl_->cameraTexture; }

void GLRenderBackend::setCameraTransform(const float matrix[16]) {
  std::memcpy(impl_->cameraTransform, matrix, sizeof(impl_->cameraTransform));
}

TextureHandle GLRenderBackend::importExternalFrame(NativeImageHandle /*cameraFrame*/) {
  // Ignored — the caller already bound the latest camera frame into
  // cameraTexture via SurfaceTexture.updateTexImage() on the GL thread
  // before calling this (see GLRenderBackend.h's class comment / ai_plans/04
  // §A.3, the one platform-specific asymmetry in this contract).
  Impl& impl = *impl_;
  if (!impl.initialized) return nullptr;
  static GLTextureHandle kCameraHandle;  // sentinel — content unused, see struct comment
  return &kCameraHandle;
}

void GLRenderBackend::beginFrame() {
  Impl& impl = *impl_;
  impl.frameValid = impl.initialized;
}

void GLRenderBackend::runPass(PassId pass, TextureHandle src, TextureHandle /*dstFbo*/,
                               const EffectParamsBlock& params) {
  Impl& impl = *impl_;
  if (!impl.frameValid) return;

  EffectParamsGPU gpu = toGPU(params);
  glBindBuffer(GL_UNIFORM_BUFFER, impl.effectParamsUbo);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(gpu), &gpu);

  switch (pass) {
    case PassId::ColorCorrection: {
      if (!src) return;
      glBindFramebuffer(GL_FRAMEBUFFER, impl.colorCorrectFbo);
      glViewport(0, 0, impl.width, impl.height);
      glUseProgram(impl.colorCorrectProgram);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, impl.cameraTexture);
      glUniform1i(glGetUniformLocation(impl.colorCorrectProgram, "uCamera"), 0);
      glUniform2f(glGetUniformLocation(impl.colorCorrectProgram, "uTexel"), 1.0f / static_cast<float>(impl.width),
                  1.0f / static_cast<float>(impl.height));
      glUniformMatrix4fv(glGetUniformLocation(impl.colorCorrectProgram, "uCameraTransform"), 1, GL_FALSE,
                         impl.cameraTransform);
      Impl::drawFullscreenTriangle();
      break;
    }
    case PassId::Vignette: {
      glBindFramebuffer(GL_FRAMEBUFFER, impl.vignetteFbo);
      glViewport(0, 0, impl.width, impl.height);
      glUseProgram(impl.vignetteProgram);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, impl.colorCorrectTex);
      glUniform1i(glGetUniformLocation(impl.vignetteProgram, "uSrc"), 0);
      Impl::drawFullscreenTriangle();
      break;
    }
    case PassId::Particles: {
      // Base composite blit — same role as Metal's blitPSO pass onto
      // particleCompositeRGBA.
      glBindFramebuffer(GL_FRAMEBUFFER, impl.particleCompositeFbo);
      glViewport(0, 0, impl.width, impl.height);
      glUseProgram(impl.blitProgram);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, impl.vignetteTex);
      glUniform1i(glGetUniformLocation(impl.blitProgram, "uSrc"), 0);
      Impl::drawFullscreenTriangle();

      if ((gpu.effectMask & kEffectParticles) != 0) {
        double elapsed = monotonicSeconds() - impl.startTime;
        impl.particles->computeInstances(static_cast<float>(elapsed), params.particleIntensity,
                                          impl.instanceScratch);
        glBindBuffer(GL_ARRAY_BUFFER, impl.particleInstanceVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(impl.instanceScratch.size() *
                                              sizeof(lumacore::render::particles::ParticleInstance)),
                     impl.instanceScratch.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE,
                               sizeof(lumacore::render::particles::ParticleInstance), nullptr);
        glVertexAttribDivisor(0, 1);

        glUseProgram(impl.particleProgram);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, impl.particles->count());
        glDisable(GL_BLEND);

        glVertexAttribDivisor(0, 0);
        glDisableVertexAttribArray(0);
      }
      break;
    }
  }
}

TextureHandle GLRenderBackend::endFrame() {
  Impl& impl = *impl_;
  if (!impl.frameValid) return nullptr;
  impl.frameValid = false;

  // Presentation: direct blit into the ANativeWindow-bound EGLSurface, then
  // swap — this *is* the show-the-frame step on Android (push model), not a
  // separate CPU pixel-buffer handoff like iOS's exportForPreview() (see
  // GLRenderBackend.h class comment).
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, impl.width, impl.height);
  glUseProgram(impl.blitProgram);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, impl.particleCompositeTex);
  glUniform1i(glGetUniformLocation(impl.blitProgram, "uSrc"), 0);
  Impl::drawFullscreenTriangle();
  eglSwapBuffers(impl.display, impl.windowSurface);

  impl.compositeHandle.texture = impl.particleCompositeTex;
  return &impl.compositeHandle;
}

PlatformImageHandle GLRenderBackend::exportForPreview(TextureHandle) {
  // Never actually called on Android: lumacore_render_frame only invokes
  // RenderPipeline::exportForPreview() when the caller passed a non-null
  // outPreviewImage, and the Android JNI path (nativeRenderFrame) passes
  // nullptr there — endFrame() above already presented the frame directly.
  return nullptr;
}

PlatformImageHandle GLRenderBackend::exportForEncoder(TextureHandle h) {
  auto* handle = static_cast<GLTextureHandle*>(h);
  if (!handle || !handle->texture) return nullptr;
  Impl& impl = *impl_;

  // GPU RGB->YCbCr conversion (1:1 port of the nv12YFS/nv12CbCrFS math) into
  // the two persistent offscreen targets — mirrors iOS's non-zero-copy CPU
  // readback encoder path (ai_plans/04 §B.1/§A.2).
  glBindFramebuffer(GL_FRAMEBUFFER, impl.nv12YFbo);
  glViewport(0, 0, impl.width, impl.height);
  glUseProgram(impl.nv12YProgram);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, handle->texture);
  glUniform1i(glGetUniformLocation(impl.nv12YProgram, "uSrc"), 0);
  Impl::drawFullscreenTriangle();

  int halfW = impl.width / 2;
  int halfH = impl.height / 2;
  glBindFramebuffer(GL_FRAMEBUFFER, impl.nv12CbCrFbo);
  glViewport(0, 0, halfW, halfH);
  glUseProgram(impl.nv12CbCrProgram);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, handle->texture);
  glUniform1i(glGetUniformLocation(impl.nv12CbCrProgram, "uSrc"), 0);
  Impl::drawFullscreenTriangle();

  // GL_RGBA/GL_UNSIGNED_BYTE is the one glReadPixels combination guaranteed
  // by GLES 3.0 regardless of the framebuffer's actual internal format — R8/
  // RG8 attachments still round-trip through it (extra channels come back
  // as 0/1), avoiding an implementation-defined-format query for a path
  // that only runs while a recording is active, not on every preview frame.
  std::vector<uint8_t> yRgba(static_cast<size_t>(impl.width) * static_cast<size_t>(impl.height) * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, impl.nv12YFbo);
  glReadPixels(0, 0, impl.width, impl.height, GL_RGBA, GL_UNSIGNED_BYTE, yRgba.data());

  std::vector<uint8_t> uvRgba(static_cast<size_t>(halfW) * static_cast<size_t>(halfH) * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, impl.nv12CbCrFbo);
  glReadPixels(0, 0, halfW, halfH, GL_RGBA, GL_UNSIGNED_BYTE, uvRgba.data());

  // One contiguous allocation — yPlane points at its start, uvPlane at the
  // Y-plane's end. Released by lumacore_api.cpp's releaseEncoderExport()
  // (`delete[] buf->yPlane; delete buf;`), see EncoderSession.h.
  size_t ySize = static_cast<size_t>(impl.width) * static_cast<size_t>(impl.height);
  size_t uvPixels = static_cast<size_t>(halfW) * static_cast<size_t>(halfH);
  auto* bytes = new uint8_t[ySize + uvPixels * 2];
  uint8_t* yOut = bytes;
  uint8_t* uvOut = bytes + ySize;
  // glReadPixels returns rows bottom-to-top (row 0 = the framebuffer's
  // bottom row, standard GL convention) — video/NV12 rows are top-to-bottom.
  // The live preview never hits this: eglSwapBuffers/the display compositor
  // handles that orientation without a CPU readback, so this mismatch only
  // ever showed up in recorded output (looked flipped, not in the preview).
  // Flip while repacking rather than adding a pass, since we're already
  // touching every byte here.
  for (int row = 0; row < impl.height; ++row) {
    const uint8_t* srcRow = yRgba.data() + static_cast<size_t>(impl.height - 1 - row) * impl.width * 4;
    uint8_t* dstRow = yOut + static_cast<size_t>(row) * impl.width;
    for (int col = 0; col < impl.width; ++col) dstRow[col] = srcRow[col * 4];
  }
  for (int row = 0; row < halfH; ++row) {
    const uint8_t* srcRow = uvRgba.data() + static_cast<size_t>(halfH - 1 - row) * halfW * 4;
    uint8_t* dstRow = uvOut + static_cast<size_t>(row) * halfW * 2;
    for (int col = 0; col < halfW; ++col) {
      dstRow[col * 2 + 0] = srcRow[col * 4 + 0];
      dstRow[col * 2 + 1] = srcRow[col * 4 + 1];
    }
  }

  auto* buf = new lumacore::encode::NativeNV12Buffer();
  buf->yPlane = yOut;
  buf->yStride = static_cast<size_t>(impl.width);
  buf->uvPlane = uvOut;
  buf->uvStride = static_cast<size_t>(impl.width);
  buf->width = impl.width;
  buf->height = impl.height;
  return buf;
}

void GLRenderBackend::destroy() { impl_->teardown(); }

}  // namespace lumacore::render::gl
