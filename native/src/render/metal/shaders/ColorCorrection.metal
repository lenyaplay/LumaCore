#include <metal_stdlib>
#include "Common.h"
#include "ShaderTypes.h"
using namespace metal;

// Fuses the YCbCr->RGB import with color correction — the only pass whose
// "src" is a pair of Y/CbCr textures instead of one RGBA texture (see
// ai_plans/03-ios-metal-render-pipeline.md §2).
fragment float4 colorCorrectFS(VSOut in [[stage_in]],
                                texture2d<float> yTex [[texture(0)]],
                                texture2d<float> cbcrTex [[texture(1)]],
                                constant EffectParamsGPU& p [[buffer(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  float y = yTex.sample(s, in.uv).r;
  float2 cbcr = cbcrTex.sample(s, in.uv).rg;
  float3 rgb = ycbcrToRGB(y, cbcr.x, cbcr.y);
  if (p.effectMask & LUMACORE_EFFECT_COLOR_CORRECTION) {
    rgb = (rgb - 0.5) * p.contrast + 0.5 + p.brightness;
    float luma = dot(rgb, float3(0.299, 0.587, 0.114));
    rgb = mix(float3(luma), rgb, p.saturation);
  }
  return float4(saturate(rgb), 1.0);
}
