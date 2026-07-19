#pragma once

// GLSL ES 3.0 source, embedded as C string constants (ai_plans/04-android-
// camerax-gl-pipeline.md §B.3 decision (b) — no Android-assets/AAssetManager
// JNI plumbing for shaders that are fixed at build time anyway). This is the
// single source of truth for each shader; there is no separate .glsl file
// generating this one, to avoid two copies drifting apart.
//
// Mechanical port of native/src/render/metal/shaders/*.metal — see
// GLRenderBackend.cpp for the pass structure these are wired into. One
// deliberate difference throughout: GLSL ES 3.0 has no 64-bit integer type,
// so effectMask arrives truncated to int32 (see GLRenderBackend.cpp's
// EffectParamsGPU) — safe today since all five effect bits (0x1..0x10) fit
// in 32 bits (ai_plans/04 context section).

namespace lumacore::render::gl::shaders {

// Shared by every RGBA->RGBA (and the camera-import) pass — no vertex
// buffer needed, position/uv derived from gl_VertexID (1:1 port of
// Common.metal's fullscreenTriangleVS).
inline constexpr const char* kFullscreenVertexSource = R"GLSL(#version 300 es
out vec2 vUv;
void main() {
  vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
  vUv = vec2(uv.x, 1.0 - uv.y);
}
)GLSL";

// Fuses the camera (samplerExternalOES, already RGB — no separate Y/CbCr
// import unlike iOS) with color correction/sepia/edge-detection. 1:1 port of
// ColorCorrection.metal, with edge-detection luma sampled from RGB taps
// instead of a dedicated Y-plane (see ai_plans/04 context section).
inline constexpr const char* kColorCorrectFragmentSource = R"GLSL(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
precision highp int;

in vec2 vUv;
uniform samplerExternalOES uCamera;
uniform vec2 uTexel;
// SurfaceTexture.getTransformMatrix() — maps our output-space UV to the
// camera buffer's actual sensor-native layout (rotation/mirroring the sensor
// orientation requires, ai_plans/04 §C follow-up). Applied per-tap (not just
// once for the center sample) so the edge-detection Sobel offsets below are
// also correctly oriented — length(vec2(gx,gy)) is rotation-invariant, so
// this stays correct even under a 90-degree transform.
uniform mat4 uCameraTransform;

layout(std140) uniform EffectParams {
  vec4 uBrightnessContrastSaturationPad0;
  vec4 uVignetteRadiusSoftnessParticleIntensityPad1;
  ivec4 uEffectMaskPad2;
  vec4 uSepiaEdgeThresholdEdgeIntensityPad3;
};

out vec4 fragColor;

float lumaOf(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }

vec4 sampleCam(vec2 uv) { return texture(uCamera, (uCameraTransform * vec4(uv, 0.0, 1.0)).xy); }

void main() {
  vec3 rgb = sampleCam(vUv).rgb;
  int effectMask = uEffectMaskPad2.x;

  if ((effectMask & 0x1) != 0) {
    float brightness = uBrightnessContrastSaturationPad0.x;
    float contrast = uBrightnessContrastSaturationPad0.y;
    float saturation = uBrightnessContrastSaturationPad0.z;
    rgb = (rgb - 0.5) * contrast + 0.5 + brightness;
    rgb = mix(vec3(lumaOf(rgb)), rgb, saturation);
  }

  // Pure B&W is already reachable via saturation=0 above. Sepia tints
  // toward a warm tone instead of desaturating.
  if ((effectMask & 0x8) != 0) {
    float sepiaAmount = uSepiaEdgeThresholdEdgeIntensityPad3.x;
    float luma = lumaOf(rgb);
    vec3 sepia = vec3(luma * 1.07, luma * 0.74, luma * 0.43);
    rgb = mix(rgb, clamp(sepia, 0.0, 1.0), sepiaAmount);
  }

  if ((effectMask & 0x10) != 0) {
    float edgeThreshold = uSepiaEdgeThresholdEdgeIntensityPad3.y;
    float edgeIntensity = uSepiaEdgeThresholdEdgeIntensityPad3.z;

    // Sobel on raw camera taps (not the possibly color-corrected/sepia'd
    // `rgb` above) so edge strength doesn't shift as other toggles change —
    // mirrors sampling the untouched Y-plane on iOS.
    float tl = lumaOf(sampleCam(vUv + uTexel * vec2(-1.0, -1.0)).rgb);
    float tc = lumaOf(sampleCam(vUv + uTexel * vec2(0.0, -1.0)).rgb);
    float tr = lumaOf(sampleCam(vUv + uTexel * vec2(1.0, -1.0)).rgb);
    float ml = lumaOf(sampleCam(vUv + uTexel * vec2(-1.0, 0.0)).rgb);
    float mr = lumaOf(sampleCam(vUv + uTexel * vec2(1.0, 0.0)).rgb);
    float bl = lumaOf(sampleCam(vUv + uTexel * vec2(-1.0, 1.0)).rgb);
    float bc = lumaOf(sampleCam(vUv + uTexel * vec2(0.0, 1.0)).rgb);
    float br = lumaOf(sampleCam(vUv + uTexel * vec2(1.0, 1.0)).rgb);

    float gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
    float gy = -tl - 2.0 * tc - tr + bl + 2.0 * bc + br;
    float edgeMag = clamp(length(vec2(gx, gy)) / 0.8, 0.0, 1.0);
    float edge = smoothstep(edgeThreshold, edgeThreshold + 0.06, edgeMag);

    vec3 posterized = floor(rgb * 4.0 + 0.5) / 4.0;
    vec3 paper = mix(posterized, vec3(1.0), 0.15);
    vec3 comic = mix(paper, vec3(0.0), edge);
    rgb = mix(rgb, comic, edgeIntensity);
  }

  fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)GLSL";

// Radial falloff, RGBA -> RGBA. 1:1 port of Vignette.metal.
inline constexpr const char* kVignetteFragmentSource = R"GLSL(#version 300 es
precision highp float;
precision highp int;

in vec2 vUv;
uniform sampler2D uSrc;

layout(std140) uniform EffectParams {
  vec4 uBrightnessContrastSaturationPad0;
  vec4 uVignetteRadiusSoftnessParticleIntensityPad1;
  ivec4 uEffectMaskPad2;
  vec4 uSepiaEdgeThresholdEdgeIntensityPad3;
};

out vec4 fragColor;

void main() {
  vec4 c = texture(uSrc, vUv);
  int effectMask = uEffectMaskPad2.x;
  if ((effectMask & 0x2) != 0) {
    float radius = uVignetteRadiusSoftnessParticleIntensityPad1.x;
    float softness = uVignetteRadiusSoftnessParticleIntensityPad1.y;
    float d = distance(vUv, vec2(0.5));
    float v = smoothstep(radius, max(radius - softness, 0.0), d);
    c.rgb *= v;
  }
  fragColor = c;
}
)GLSL";

// Plain RGBA passthrough — used for the base composite blit before
// particles, and for the final present-to-screen blit in endFrame(). 1:1
// port of Particles.metal's blitFS.
inline constexpr const char* kBlitFragmentSource = R"GLSL(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSrc;
out vec4 fragColor;
void main() { fragColor = texture(uSrc, vUv); }
)GLSL";

// Instanced particle quads (divisor-1 vertex attribute, not a storage-buffer
// read like Metal's particleVS — GLSL ES 3.0 has no SSBOs). 1:1 port of
// Particles.metal's particleVS otherwise.
inline constexpr const char* kParticleVertexSource = R"GLSL(#version 300 es
layout(location = 0) in vec4 aInstance; // x, y, alpha, size
out vec2 vLocalUv;
out float vAlpha;
void main() {
  vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vec2 uv = corner * 2.0 - 1.0;
  vec2 worldPos = aInstance.xy + uv * aInstance.w;
  gl_Position = vec4(worldPos * 2.0 - 1.0, 0.0, 1.0);
  vLocalUv = uv;
  vAlpha = aInstance.z;
}
)GLSL";

// 1:1 port of Particles.metal's particleFS.
inline constexpr const char* kParticleFragmentSource = R"GLSL(#version 300 es
precision highp float;
in vec2 vLocalUv;
in float vAlpha;
out vec4 fragColor;
void main() {
  float r = length(vLocalUv);
  float falloff = smoothstep(1.0, 0.0, r);
  fragColor = vec4(1.0, 1.0, 1.0, vAlpha * falloff);
}
)GLSL";

// RGB->Y, full resolution. Rendered into an R8 FBO; the bilinear downsample
// for CbCr happens for free on the texture sample in kNv12CbCrFragmentSource
// (viewport set to half-size by the caller). 1:1 port of Particles.metal's
// nv12YFS/nv12CbCrFS math (BT.601 full-range, matches
// kCVPixelFormatType_420YpCbCr8BiPlanarFullRange on iOS).
inline constexpr const char* kNv12YFragmentSource = R"GLSL(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSrc;
out vec4 fragColor;
void main() {
  vec3 rgb = texture(uSrc, vUv).rgb;
  float y = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
  fragColor = vec4(y, 0.0, 0.0, 1.0);
}
)GLSL";

inline constexpr const char* kNv12CbCrFragmentSource = R"GLSL(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSrc;
out vec4 fragColor;
void main() {
  vec3 rgb = texture(uSrc, vUv).rgb;
  float cb = -0.168736 * rgb.r - 0.331264 * rgb.g + 0.5 * rgb.b + 0.5;
  float cr = 0.5 * rgb.r - 0.418688 * rgb.g - 0.081312 * rgb.b + 0.5;
  fragColor = vec4(cb, cr, 0.0, 1.0);
}
)GLSL";

}  // namespace lumacore::render::gl::shaders
