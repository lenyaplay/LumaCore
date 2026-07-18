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
};

static_assert(sizeof(EffectParamsBlock) % 16 == 0,
              "EffectParamsBlock must be 16-byte aligned for std140/Metal constant buffers");

inline EffectParamsBlock toEffectParamsBlock(const LumaEffectParams& params) {
  EffectParamsBlock block{};
  block.brightness = params.brightness;
  block.contrast = params.contrast;
  block.saturation = params.saturation;
  block.vignetteRadius = params.vignetteRadius;
  block.vignetteSoftness = params.vignetteSoftness;
  block.particleIntensity = params.particleIntensity;
  block.effectMask = params.effectMask;
  return block;
}

}  // namespace lumacore::render
