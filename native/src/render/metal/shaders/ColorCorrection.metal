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

  // Pure B&W is already reachable via saturation=0 above — no new code
  // needed for that. Sepia tints toward a warm tone instead of desaturating.
  if (p.effectMask & LUMACORE_EFFECT_SEPIA) {
    float luma = dot(rgb, float3(0.299, 0.587, 0.114));
    float3 sepia = float3(luma * 1.07, luma * 0.74, luma * 0.43);
    rgb = mix(rgb, saturate(sepia), p.sepiaAmount);
  }

  if (p.effectMask & LUMACORE_EFFECT_EDGES) {
    // Sobel on the raw Y plane (already luma, no extra computation) rather
    // than the possibly color-corrected/sepia'd `rgb` above, so edge
    // strength doesn't shift as other toggles change.
    float2 texel = float2(1.0 / float(yTex.get_width()), 1.0 / float(yTex.get_height()));
    float tl = yTex.sample(s, in.uv + texel * float2(-1, -1)).r;
    float tc = yTex.sample(s, in.uv + texel * float2(0, -1)).r;
    float tr = yTex.sample(s, in.uv + texel * float2(1, -1)).r;
    float ml = yTex.sample(s, in.uv + texel * float2(-1, 0)).r;
    float mr = yTex.sample(s, in.uv + texel * float2(1, 0)).r;
    float bl = yTex.sample(s, in.uv + texel * float2(-1, 1)).r;
    float bc = yTex.sample(s, in.uv + texel * float2(0, 1)).r;
    float br = yTex.sample(s, in.uv + texel * float2(1, 1)).r;

    float gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
    float gy = -tl - 2.0 * tc - tr + bl + 2.0 * bc + br;
    // Sobel magnitude on [0,1] luma can reach ~5.66 at a hard step edge;
    // /2.0 is an empirical normalization keeping edgeThreshold usable in [0,1].
    float edgeMag = saturate(length(float2(gx, gy)) / 2.0);
    float edge = smoothstep(p.edgeThreshold, p.edgeThreshold + 0.1, edgeMag);

    float3 posterized = floor(rgb * 4.0 + 0.5) / 4.0;
    float3 paper = mix(float3(0.95), posterized, 0.6);
    float3 comic = mix(paper, float3(0.0), edge);
    rgb = mix(rgb, comic, p.edgeIntensity);
  }

  return float4(saturate(rgb), 1.0);
}
