#include <metal_stdlib>
#include "Common.h"
using namespace metal;

// Fullscreen-triangle vertex function, no vertex buffer needed (classic
// no-VBO trick keyed off vertex_id). Shared by every RGBA->RGBA pass.
vertex VSOut fullscreenTriangleVS(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
  out.uv = float2(uv.x, 1.0 - uv.y);
  return out;
}

// BT.601 full-range conversions — matches
// kCVPixelFormatType_420YpCbCr8BiPlanarFullRange (camera's native format).
float3 ycbcrToRGB(float y, float cb, float cr) {
  cb -= 0.5;
  cr -= 0.5;
  return float3(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb);
}

float3 rgbToYCbCr(float3 rgb) {
  float yy = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
  float cb = -0.168736 * rgb.r - 0.331264 * rgb.g + 0.5 * rgb.b + 0.5;
  float cr = 0.5 * rgb.r - 0.418688 * rgb.g - 0.081312 * rgb.b + 0.5;
  return float3(yy, cb, cr);
}
