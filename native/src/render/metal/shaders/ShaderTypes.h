#ifndef LUMACORE_SHADER_TYPES_H
#define LUMACORE_SHADER_TYPES_H

// Shared between MSL (.metal) and Obj-C++ (.mm) via #if __METAL_VERSION__ —
// standard Apple pattern. This is a byte-for-byte mirror of EffectParamsBlock
// in render/EffectParams.h. It does NOT #include that header directly — that
// header pulls in <cstdint>/lumacore_api.h, which are not MSL-compatible.
// Keep these two structs in sync by hand:
//   render/EffectParams.h::EffectParamsBlock <-> ShaderTypes.h::EffectParamsGPU
// If you add/reorder a field in one, update the other.

#ifdef __METAL_VERSION__
#include <metal_stdlib>
#else
#include <simd/simd.h>
#endif

struct alignas(16) EffectParamsGPU {
  float brightness, contrast, saturation, _pad0;
  float vignetteRadius, vignetteSoftness, particleIntensity, _pad1;
  long effectMask, _pad2;
  float sepiaAmount, edgeThreshold, edgeIntensity, _pad3;
};

// effectMask bits (convention fixed here, not in ARCHITECTURE.md):
//   0x1 = ColorCorrection, 0x2 = Vignette, 0x4 = Particles, 0x8 = Sepia,
//   0x10 = Edges. Default 0x7 (ColorCorrection+Vignette+Particles on; Sepia
//   and Edges are opt-in, off by default).
#define LUMACORE_EFFECT_COLOR_CORRECTION 0x1
#define LUMACORE_EFFECT_VIGNETTE 0x2
#define LUMACORE_EFFECT_PARTICLES 0x4
#define LUMACORE_EFFECT_SEPIA 0x8
#define LUMACORE_EFFECT_EDGES 0x10

struct alignas(16) ParticleInstanceGPU {
  float x, y, alpha, size;
};

#endif
