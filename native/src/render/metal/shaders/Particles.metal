#include <metal_stdlib>
#include "Common.h"
#include "ShaderTypes.h"
using namespace metal;

// Base composite blit (RGBA -> RGBA passthrough onto the particle-composite target).
fragment float4 blitFS(VSOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  return src.sample(s, in.uv);
}

// Instanced particle quads, additive blending (sourceAlpha, one — particles
// "glow" rather than alpha-composite over the scene). Whether this draw call
// is issued at all (effectMask & LUMACORE_EFFECT_PARTICLES) is decided by
// MetalRenderBackend::runPass, not here — the geometry is a whole draw call,
// not a per-pixel branch like ColorCorrection/Vignette.
struct ParticleVSOut {
  float4 position [[position]];
  float2 localUV;
  float alpha;
};

vertex ParticleVSOut particleVS(uint vid [[vertex_id]], uint iid [[instance_id]],
                                 constant ParticleInstanceGPU* instances [[buffer(0)]]) {
  ParticleInstanceGPU p = instances[iid];
  float2 corner = float2((vid << 1) & 2, vid & 2);
  float2 uv = corner * 2.0 - 1.0;
  float2 worldPos = float2(p.x, p.y) + uv * p.size;
  ParticleVSOut out;
  out.position = float4(worldPos * 2.0 - 1.0, 0.0, 1.0);
  out.localUV = uv;
  out.alpha = p.alpha;
  return out;
}

fragment float4 particleFS(ParticleVSOut in [[stage_in]]) {
  float r = length(in.localUV);
  float falloff = smoothstep(1.0, 0.0, r);
  return float4(1.0, 1.0, 1.0, in.alpha * falloff);
}

// Final conversion back to NV12 (Y-plane full size, CbCr-plane at half-size
// viewport — the bilinear downsample happens for free on the texture sample).
fragment float4 nv12YFS(VSOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  return float4(rgbToYCbCr(src.sample(s, in.uv).rgb).r, 0, 0, 1);
}

fragment float4 nv12CbCrFS(VSOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  float3 ycbcr = rgbToYCbCr(src.sample(s, in.uv).rgb);
  return float4(ycbcr.gb, 0, 1);
}
