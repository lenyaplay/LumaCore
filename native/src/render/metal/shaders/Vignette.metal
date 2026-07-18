#include <metal_stdlib>
#include "Common.h"
#include "ShaderTypes.h"
using namespace metal;

// Radial falloff, RGBA -> RGBA.
fragment float4 vignetteFS(VSOut in [[stage_in]],
                            texture2d<float> src [[texture(0)]],
                            constant EffectParamsGPU& p [[buffer(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  float4 c = src.sample(s, in.uv);
  if (p.effectMask & LUMACORE_EFFECT_VIGNETTE) {
    float d = distance(in.uv, float2(0.5));
    float v = smoothstep(p.vignetteRadius, max(p.vignetteRadius - p.vignetteSoftness, 0.0), d);
    c.rgb *= v;
  }
  return c;
}
