#include "ParticleSystem.h"

#include <cmath>

namespace lumacore::render::particles {

namespace {

// xorshift32 — small, fast, deterministic for a given seed. Not
// cryptographic; just needs to spread particle attributes without visible
// patterning.
class Xorshift32 {
 public:
  explicit Xorshift32(uint32_t seed) : state_(seed != 0 ? seed : 0x9E3779B9u) {}

  uint32_t next() {
    uint32_t x = state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state_ = x;
    return x;
  }

  // Uniform float in [lo, hi).
  float nextFloat(float lo, float hi) {
    float t = static_cast<float>(next()) / static_cast<float>(UINT32_MAX);
    return lo + t * (hi - lo);
  }

 private:
  uint32_t state_;
};

constexpr float kPi = 3.14159265358979323846f;

// Wraps v into [0, 1) — fmod alone can return a negative result for negative
// inputs, which would put a particle off-screen instead of wrapping it.
float wrap01(float v) {
  v = std::fmod(v, 1.0f);
  if (v < 0.0f) v += 1.0f;
  return v;
}

}  // namespace

ParticleSystem::ParticleSystem(int count, uint32_t seed) {
  seeds_.reserve(static_cast<size_t>(count));
  Xorshift32 rng(seed);
  for (int i = 0; i < count; ++i) {
    ParticleSeed s{};
    s.x0 = rng.nextFloat(0.0f, 1.0f);
    s.y0 = rng.nextFloat(0.0f, 1.0f);
    s.vx = rng.nextFloat(-0.05f, 0.05f);
    s.vy = rng.nextFloat(-0.05f, 0.05f);
    s.lifetimeSec = rng.nextFloat(2.0f, 5.0f);
    s.phaseOffset = rng.nextFloat(0.0f, s.lifetimeSec);
    s.size = rng.nextFloat(0.01f, 0.04f);
    seeds_.push_back(s);
  }
}

int ParticleSystem::count() const { return static_cast<int>(seeds_.size()); }

void ParticleSystem::computeInstances(float elapsedSeconds, float intensity,
                                       std::vector<ParticleInstance>& outInstances) const {
  outInstances.resize(seeds_.size());
  for (size_t i = 0; i < seeds_.size(); ++i) {
    const ParticleSeed& seed = seeds_[i];
    float age = std::fmod(elapsedSeconds + seed.phaseOffset, seed.lifetimeSec);
    if (age < 0.0f) age += seed.lifetimeSec;
    float t = age / seed.lifetimeSec;

    ParticleInstance& inst = outInstances[i];
    inst.x = wrap01(seed.x0 + seed.vx * age);
    inst.y = wrap01(seed.y0 + seed.vy * age);
    inst.alpha = intensity * std::sin(t * kPi);
    inst.size = seed.size * (0.5f + 0.5f * std::sin(t * kPi));
  }
}

}  // namespace lumacore::render::particles
