#pragma once
#include <metal_stdlib>
using namespace metal;

// Shared across all passes. Declarations only — definitions live in
// Common.metal as non-`inline` functions. `inline` functions do not survive
// metallib's cross-.air linking (each .metal file compiles to its own AIR
// module before metallib merges them, and an `inline` function's symbol is
// not exported for other modules to resolve) — verified empirically, keep
// these non-inline if you touch this file.
struct VSOut {
  float4 position [[position]];
  float2 uv;
};

float3 ycbcrToRGB(float y, float cb, float cr);
float3 rgbToYCbCr(float3 rgb);
