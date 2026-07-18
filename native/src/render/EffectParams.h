#pragma once

#include <cstdint>

#include "api/lumacore_api.h"

namespace lumacore::render {

// GPU-side mirror of LumaEffectParams. Layout is explicit (alignas/padding) so
// the same byte block can be copied into a GL std140 UBO (glBufferData) and a
// Metal constant buffer (newBufferWithBytes:) without platform-specific
// marshaling. See ARCHITECTURE.md §2.
struct alignas(16) EffectParamsBlock {
  float brightness;
  float contrast;
  float saturation;
  float _pad0;

  float vignetteRadius;
  float vignetteSoftness;
  float particleIntensity;
  float _pad1;

  int64_t effectMask;
  int64_t _pad2;

  float sepiaAmount;
  float edgeThreshold;
  float edgeIntensity;
  float _pad3;
};

static_assert(sizeof(EffectParamsBlock) % 16 == 0,
              "EffectParamsBlock must be 16-byte aligned for std140/Metal constant buffers");
static_assert(sizeof(EffectParamsBlock) == 64,
              "grew by one 16-byte block for sepia/edge-detection params — see docs/ai_plans");

inline EffectParamsBlock toEffectParamsBlock(const LumaEffectParams& params) {
  EffectParamsBlock block{};
  block.brightness = params.brightness;
  block.contrast = params.contrast;
  block.saturation = params.saturation;
  block.vignetteRadius = params.vignetteRadius;
  block.vignetteSoftness = params.vignetteSoftness;
  block.particleIntensity = params.particleIntensity;
  block.effectMask = params.effectMask;
  block.sepiaAmount = params.sepiaAmount;
  block.edgeThreshold = params.edgeThreshold;
  block.edgeIntensity = params.edgeIntensity;
  return block;
}

}  // namespace lumacore::render
