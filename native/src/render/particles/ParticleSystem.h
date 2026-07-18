#pragma once

#include <cstdint>
#include <vector>

namespace lumacore::render::particles {

// Static per-particle attributes, computed once from a seeded PRNG and never
// mutated frame-to-frame. Shared across all backends (ARCHITECTURE.md §2).
struct ParticleSeed {
  float x0, y0;        // normalized start position [0,1]^2
  float vx, vy;         // drift in screen units/sec
  float lifetimeSec;    // 2.0-5.0s, varies per particle so they don't all "tick" together
  float phaseOffset;    // 0..lifetimeSec, shifts each particle's start time
  float size;           // sprite radius in UV units
};

// GPU instance-buffer layout (16 bytes) — mirrors ShaderTypes.h::ParticleInstanceGPU.
struct alignas(16) ParticleInstance {
  float x, y, alpha, size;
};

class ParticleSystem {
 public:
  ParticleSystem(int count, uint32_t seed);

  int count() const;

  // Pure function — deterministic, no side effects, no locking. Called once
  // per frame from MetalRenderBackend::runPass(Particles, ...).
  void computeInstances(float elapsedSeconds, float intensity,
                         std::vector<ParticleInstance>& outInstances) const;

 private:
  std::vector<ParticleSeed> seeds_;
};

}  // namespace lumacore::render::particles
