#import "MetalRenderBackend.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <vector>

#include "render/particles/ParticleSystem.h"
#include "shaders/ShaderTypes.h"

namespace lumacore::render::metal {

namespace {

EffectParamsGPU toGPU(const EffectParamsBlock& p) {
  EffectParamsGPU g{};
  g.brightness = p.brightness;
  g.contrast = p.contrast;
  g.saturation = p.saturation;
  g._pad0 = 0.0f;
  g.vignetteRadius = p.vignetteRadius;
  g.vignetteSoftness = p.vignetteSoftness;
  g.particleIntensity = p.particleIntensity;
  g._pad1 = 0.0f;
  g.effectMask = p.effectMask;
  g._pad2 = 0;
  return g;
}

}  // namespace

// Private detail — does not change the public TextureHandle=void* contract
// of IRenderBackend (ai_plans/03-ios-metal-render-pipeline.md §3).
struct MetalTextureHandle {
  id<MTLTexture> primary;    // RGBA intermediate, or Y-plane (YCbCr/NV12 handles)
  id<MTLTexture> secondary;  // nil except: camera import (CbCr) and NV12 target (CbCr)
  bool isYCbCr = false;
  CVPixelBufferRef owningBuffer = nullptr;  // only for pool-derived handles (+1 owned)
};

struct MetalRenderBackend::Impl {
  id<MTLDevice> device;
  id<MTLCommandQueue> commandQueue;
  id<MTLLibrary> library;
  id<MTLCommandBuffer> currentCommandBuffer;

  id<MTLRenderPipelineState> colorCorrectPSO;
  id<MTLRenderPipelineState> vignettePSO;
  id<MTLRenderPipelineState> blitPSO;
  id<MTLRenderPipelineState> particlePSO;
  id<MTLRenderPipelineState> nv12YPSO;
  id<MTLRenderPipelineState> nv12CbCrPSO;

  CVMetalTextureCacheRef textureCache = nullptr;
  CVPixelBufferPoolRef previewPool = nullptr;   // preview export lifecycle
  CVPixelBufferPoolRef encoderPool = nullptr;   // separate: encoder export lifecycle

  id<MTLTexture> colorCorrectRGBA;
  id<MTLTexture> vignetteRGBA;
  id<MTLTexture> particleCompositeRGBA;

  int width = 0;
  int height = 0;

  std::unique_ptr<lumacore::render::particles::ParticleSystem> particles;
  std::vector<lumacore::render::particles::ParticleInstance> instanceScratch;
  double startTime = 0.0;  // CFAbsoluteTimeGetCurrent() at initialize() — particle clock

  // Handles allocated this frame, freed at the start of the next beginFrame()
  // — simple accumulator, not a full allocator, sufficient at this volume.
  std::vector<MetalTextureHandle*> pendingHandles;
  MetalTextureHandle* currentPreviewTarget = nullptr;
  bool frameValid = false;

  static void freeHandle(MetalTextureHandle* h) {
    if (!h) return;
    if (h->owningBuffer) CVPixelBufferRelease(h->owningBuffer);
    delete h;
  }

  void teardown() {
    for (auto* h : pendingHandles) freeHandle(h);
    pendingHandles.clear();
    currentPreviewTarget = nullptr;
    currentCommandBuffer = nil;
    if (textureCache) {
      CVMetalTextureCacheFlush(textureCache, 0);
      CFRelease(textureCache);
      textureCache = nullptr;
    }
    if (previewPool) {
      CVPixelBufferPoolRelease(previewPool);
      previewPool = nullptr;
    }
    if (encoderPool) {
      CVPixelBufferPoolRelease(encoderPool);
      encoderPool = nullptr;
    }
  }

  ~Impl() { teardown(); }

  id<MTLTexture> textureFromPlane(CVPixelBufferRef buffer, size_t planeIndex, MTLPixelFormat format) {
    size_t w = CVPixelBufferGetWidthOfPlane(buffer, planeIndex);
    size_t h = CVPixelBufferGetHeightOfPlane(buffer, planeIndex);
    CVMetalTextureRef cvTexture = nullptr;
    CVReturn res = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, textureCache, buffer, nullptr,
                                                               format, w, h, planeIndex, &cvTexture);
    if (res != kCVReturnSuccess || !cvTexture) return nil;
    id<MTLTexture> texture = CVMetalTextureGetTexture(cvTexture);
    CFRelease(cvTexture);
    return texture;
  }

  id<MTLRenderPipelineState> makePipeline(NSString* vsName, NSString* fsName, MTLPixelFormat colorFormat,
                                           bool additiveBlend) {
    id<MTLFunction> vs = [library newFunctionWithName:vsName];
    id<MTLFunction> fs = [library newFunctionWithName:fsName];
    if (!vs || !fs) return nil;

    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    desc.colorAttachments[0].pixelFormat = colorFormat;
    if (additiveBlend) {
      desc.colorAttachments[0].blendingEnabled = YES;
      desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
      desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
      desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
      desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    NSError* err = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
    return pso;
  }

  // Single fullscreen-triangle draw pass — used by every pass except the
  // combined base-composite+particles step in runPass(Particles, ...), which
  // needs two draws (different pipeline states) sharing one encoder/load.
  void runFullscreenPass(id<MTLRenderPipelineState> pso, id<MTLTexture> colorTarget,
                          void (^configure)(id<MTLRenderCommandEncoder>)) {
    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = colorTarget;
    rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [currentCommandBuffer renderCommandEncoderWithDescriptor:rpd];
    [enc setRenderPipelineState:pso];
    if (configure) configure(enc);
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
  }
};

MetalRenderBackend::MetalRenderBackend() : impl_(std::make_unique<Impl>()) {}

MetalRenderBackend::~MetalRenderBackend() { destroy(); }

bool MetalRenderBackend::initialize(const RenderContextParams& params) {
  Impl& impl = *impl_;
  if (params.width <= 0 || params.height <= 0) return false;

  impl.device = MTLCreateSystemDefaultDevice();
  if (!impl.device) return false;

  impl.commandQueue = [impl.device newCommandQueue];
  if (!impl.commandQueue) return false;

  // Precompiled library only — never a runtime string compile. See §4 for how
  // lumacore.metallib gets bundled into Runner.
  NSURL* libURL = [[NSBundle mainBundle] URLForResource:@"lumacore" withExtension:@"metallib"];
  if (!libURL) return false;
  NSError* err = nil;
  impl.library = [impl.device newLibraryWithURL:libURL error:&err];
  if (!impl.library) return false;

  impl.colorCorrectPSO = impl.makePipeline(@"fullscreenTriangleVS", @"colorCorrectFS", MTLPixelFormatBGRA8Unorm, false);
  impl.vignettePSO = impl.makePipeline(@"fullscreenTriangleVS", @"vignetteFS", MTLPixelFormatBGRA8Unorm, false);
  impl.blitPSO = impl.makePipeline(@"fullscreenTriangleVS", @"blitFS", MTLPixelFormatBGRA8Unorm, false);
  impl.particlePSO = impl.makePipeline(@"particleVS", @"particleFS", MTLPixelFormatBGRA8Unorm, true);
  impl.nv12YPSO = impl.makePipeline(@"fullscreenTriangleVS", @"nv12YFS", MTLPixelFormatR8Unorm, false);
  impl.nv12CbCrPSO = impl.makePipeline(@"fullscreenTriangleVS", @"nv12CbCrFS", MTLPixelFormatRG8Unorm, false);
  if (!impl.colorCorrectPSO || !impl.vignettePSO || !impl.blitPSO || !impl.particlePSO || !impl.nv12YPSO ||
      !impl.nv12CbCrPSO) {
    return false;
  }

  if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, impl.device, nullptr, &impl.textureCache) !=
      kCVReturnSuccess) {
    return false;
  }

  impl.width = params.width;
  impl.height = params.height;

  NSDictionary* pixelBufferAttrs = @{
    (NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
    (NSString*)kCVPixelBufferMetalCompatibilityKey : @YES,
    (NSString*)kCVPixelBufferWidthKey : @(impl.width),
    (NSString*)kCVPixelBufferHeightKey : @(impl.height),
    (NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  NSDictionary* poolAttrs = @{(NSString*)kCVPixelBufferPoolMinimumBufferCountKey : @2};

  // Separate pools (not shared) — preview and encoder export have independent
  // lifecycles; a shared pool would create a hidden coupling between them.
  CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)poolAttrs,
                           (__bridge CFDictionaryRef)pixelBufferAttrs, &impl.previewPool);
  CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)poolAttrs,
                           (__bridge CFDictionaryRef)pixelBufferAttrs, &impl.encoderPool);
  if (!impl.previewPool || !impl.encoderPool) return false;

  MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:impl.width
                                                                                   height:impl.height
                                                                                mipmapped:NO];
  desc.storageMode = MTLStorageModePrivate;
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  impl.colorCorrectRGBA = [impl.device newTextureWithDescriptor:desc];
  impl.vignetteRGBA = [impl.device newTextureWithDescriptor:desc];
  impl.particleCompositeRGBA = [impl.device newTextureWithDescriptor:desc];
  if (!impl.colorCorrectRGBA || !impl.vignetteRGBA || !impl.particleCompositeRGBA) return false;

  impl.particles = std::make_unique<lumacore::render::particles::ParticleSystem>(200, /*seed=*/42);
  impl.startTime = CFAbsoluteTimeGetCurrent();

  return true;
}

TextureHandle MetalRenderBackend::importExternalFrame(NativeImageHandle cameraFrame) {
  Impl& impl = *impl_;
  if (!impl.frameValid || !cameraFrame) return nullptr;

  auto buffer = static_cast<CVPixelBufferRef>(cameraFrame);
  id<MTLTexture> luma = impl.textureFromPlane(buffer, 0, MTLPixelFormatR8Unorm);
  id<MTLTexture> chroma = impl.textureFromPlane(buffer, 1, MTLPixelFormatRG8Unorm);
  if (!luma || !chroma) {
    impl.frameValid = false;
    return nullptr;
  }

  auto* handle = new MetalTextureHandle();
  handle->primary = luma;
  handle->secondary = chroma;
  handle->isYCbCr = true;
  handle->owningBuffer = nullptr;  // camera frame lifetime belongs to the caller, not us
  impl.pendingHandles.push_back(handle);
  return handle;
}

void MetalRenderBackend::beginFrame() {
  Impl& impl = *impl_;

  for (auto* h : impl.pendingHandles) Impl::freeHandle(h);
  impl.pendingHandles.clear();
  impl.currentPreviewTarget = nullptr;
  impl.frameValid = false;

  impl.currentCommandBuffer = [impl.commandQueue commandBuffer];

  CVPixelBufferRef previewBuffer = nullptr;
  CVReturn res = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, impl.previewPool, &previewBuffer);
  if (res != kCVReturnSuccess || !previewBuffer) {
    return;  // pool exhausted — frame stays dropped, RenderPipeline counts it
  }

  id<MTLTexture> yTex = impl.textureFromPlane(previewBuffer, 0, MTLPixelFormatR8Unorm);
  id<MTLTexture> cbcrTex = impl.textureFromPlane(previewBuffer, 1, MTLPixelFormatRG8Unorm);
  if (!yTex || !cbcrTex) {
    CVPixelBufferRelease(previewBuffer);
    return;
  }

  auto* handle = new MetalTextureHandle();
  handle->primary = yTex;
  handle->secondary = cbcrTex;
  handle->isYCbCr = true;
  handle->owningBuffer = previewBuffer;  // +1 owned from the pool create call
  impl.pendingHandles.push_back(handle);
  impl.currentPreviewTarget = handle;
  impl.frameValid = true;
}

void MetalRenderBackend::runPass(PassId pass, TextureHandle src, TextureHandle dstFbo,
                                  const EffectParamsBlock& params) {
  (void)dstFbo;  // backend chains its own persistent intermediates internally, see §3
  Impl& impl = *impl_;
  if (!impl.frameValid) return;

  EffectParamsGPU gpu = toGPU(params);

  switch (pass) {
    case PassId::ColorCorrection: {
      auto* srcHandle = static_cast<MetalTextureHandle*>(src);
      if (!srcHandle) return;
      impl.runFullscreenPass(impl.colorCorrectPSO, impl.colorCorrectRGBA, ^(id<MTLRenderCommandEncoder> enc) {
        [enc setFragmentTexture:srcHandle->primary atIndex:0];
        [enc setFragmentTexture:srcHandle->secondary atIndex:1];
        [enc setFragmentBytes:&gpu length:sizeof(EffectParamsGPU) atIndex:0];
      });
      break;
    }
    case PassId::Vignette: {
      id<MTLTexture> input = impl.colorCorrectRGBA;
      impl.runFullscreenPass(impl.vignettePSO, impl.vignetteRGBA, ^(id<MTLRenderCommandEncoder> enc) {
        [enc setFragmentTexture:input atIndex:0];
        [enc setFragmentBytes:&gpu length:sizeof(EffectParamsGPU) atIndex:0];
      });
      break;
    }
    case PassId::Particles: {
      if (!impl.currentPreviewTarget) return;

      // Base composite blit + instanced particle quads additively on top —
      // one encoder, two draws (plan §3 counts this as one of the three
      // render-pass-encoders for this call).
      {
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = impl.particleCompositeRGBA;
        rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [impl.currentCommandBuffer renderCommandEncoderWithDescriptor:rpd];

        [enc setRenderPipelineState:impl.blitPSO];
        [enc setFragmentTexture:impl.vignetteRGBA atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        if (gpu.effectMask & LUMACORE_EFFECT_PARTICLES) {
          double elapsed = CFAbsoluteTimeGetCurrent() - impl.startTime;
          impl.particles->computeInstances(static_cast<float>(elapsed), params.particleIntensity,
                                            impl.instanceScratch);
          size_t bytes = impl.instanceScratch.size() * sizeof(lumacore::render::particles::ParticleInstance);
          id<MTLBuffer> instanceBuffer = [impl.device newBufferWithBytes:impl.instanceScratch.data()
                                                                    length:bytes
                                                                   options:MTLResourceStorageModeShared];
          [enc setRenderPipelineState:impl.particlePSO];
          [enc setVertexBuffer:instanceBuffer offset:0 atIndex:0];
          [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:3
                  instanceCount:impl.particles->count()];
        }
        [enc endEncoding];
      }

      // Final conversion back to NV12, straight into the preview target's
      // own Y/CbCr textures (see §2 — the only two conversion points).
      id<MTLTexture> composite = impl.particleCompositeRGBA;
      impl.runFullscreenPass(impl.nv12YPSO, impl.currentPreviewTarget->primary, ^(id<MTLRenderCommandEncoder> enc) {
        [enc setFragmentTexture:composite atIndex:0];
      });
      impl.runFullscreenPass(impl.nv12CbCrPSO, impl.currentPreviewTarget->secondary,
                              ^(id<MTLRenderCommandEncoder> enc) {
                                [enc setFragmentTexture:composite atIndex:0];
                              });
      break;
    }
  }
}

TextureHandle MetalRenderBackend::endFrame() {
  Impl& impl = *impl_;
  if (!impl.frameValid || !impl.currentCommandBuffer) return nullptr;

  // Synchronous commit — see §3 risk note: this removes CPU/GPU parallelism
  // between frames. Not yet measured against the 33ms/frame (30fps) budget
  // on-device; if it doesn't fit, the fast-follow is addCompletedHandler +
  // a 2-3-in-flight semaphore, deliberately not implemented here.
  [impl.currentCommandBuffer commit];
  [impl.currentCommandBuffer waitUntilCompleted];
  impl.currentCommandBuffer = nil;

  return impl.currentPreviewTarget;
}

PlatformImageHandle MetalRenderBackend::exportForPreview(TextureHandle h) {
  auto* handle = static_cast<MetalTextureHandle*>(h);
  if (!handle || !handle->owningBuffer) return nullptr;
  CVPixelBufferRetain(handle->owningBuffer);
  return handle->owningBuffer;
}

PlatformImageHandle MetalRenderBackend::exportForEncoder(TextureHandle h) {
  auto* handle = static_cast<MetalTextureHandle*>(h);
  if (!handle || !handle->owningBuffer) return nullptr;
  Impl& impl = *impl_;

  CVPixelBufferRef encoderBuffer = nullptr;
  CVReturn res = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, impl.encoderPool, &encoderBuffer);
  if (res != kCVReturnSuccess || !encoderBuffer) return nullptr;

  id<MTLTexture> dstY = impl.textureFromPlane(encoderBuffer, 0, MTLPixelFormatR8Unorm);
  id<MTLTexture> dstCbCr = impl.textureFromPlane(encoderBuffer, 1, MTLPixelFormatRG8Unorm);
  if (!dstY || !dstCbCr) {
    CVPixelBufferRelease(encoderBuffer);
    return nullptr;
  }

  // Only pays the blit cost while a recording is actually active, not on
  // every preview frame.
  id<MTLCommandBuffer> blitCmd = [impl.commandQueue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [blitCmd blitCommandEncoder];
  MTLSize ySize = MTLSizeMake(handle->primary.width, handle->primary.height, 1);
  MTLSize cbcrSize = MTLSizeMake(handle->secondary.width, handle->secondary.height, 1);
  [blit copyFromTexture:handle->primary
             sourceSlice:0
             sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:ySize
               toTexture:dstY
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit copyFromTexture:handle->secondary
             sourceSlice:0
             sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:cbcrSize
               toTexture:dstCbCr
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  [blitCmd commit];
  [blitCmd waitUntilCompleted];

  // encoderBuffer is already +1 owned from the pool create call above — that
  // is exactly the retained handle NS_RETURNS_RETAINED callers expect.
  return encoderBuffer;
}

void MetalRenderBackend::destroy() { impl_->teardown(); }

}  // namespace lumacore::render::metal
