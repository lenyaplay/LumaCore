// Pure-logic unit test, no external framework — runs on any host CI without
// devices/toolchains (ARCHITECTURE.md §5). Each check prints PASS/FAIL and the
// process exits non-zero on any failure, which is all CTest needs.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "render/particles/ParticleSystem.h"

using lumacore::render::particles::ParticleInstance;
using lumacore::render::particles::ParticleSystem;

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
  if (condition) {
    std::printf("PASS: %s\n", description);
  } else {
    std::printf("FAIL: %s\n", description);
    ++g_failures;
  }
}

constexpr int kCount = 200;

void testCountMatchesConstructor() {
  ParticleSystem system(kCount, /*seed=*/42);
  check(system.count() == kCount, "count() equals the constructor count");

  std::vector<ParticleInstance> instances;
  system.computeInstances(1.5f, /*intensity=*/1.0f, instances);
  check(static_cast<int>(instances.size()) == kCount,
        "computeInstances produces exactly count() elements");
}

void testInstancesWithinBounds() {
  ParticleSystem system(kCount, 42);
  std::vector<ParticleInstance> instances;
  system.computeInstances(1.5f, 1.0f, instances);

  bool allAlphaValid = true;
  bool allSizePositive = true;
  for (const auto& inst : instances) {
    if (inst.alpha < 0.0f || inst.alpha > 1.0f) allAlphaValid = false;
    if (inst.size <= 0.0f) allSizePositive = false;
  }
  check(allAlphaValid, "all alpha values are in [0,1]");
  check(allSizePositive, "all size values are > 0");
}

void testDeterminism() {
  ParticleSystem system(kCount, 42);
  std::vector<ParticleInstance> first;
  std::vector<ParticleInstance> second;
  system.computeInstances(1.5f, 1.0f, first);
  system.computeInstances(1.5f, 1.0f, second);

  bool identical = first.size() == second.size();
  for (size_t i = 0; identical && i < first.size(); ++i) {
    identical = first[i].x == second[i].x && first[i].y == second[i].y &&
                first[i].alpha == second[i].alpha && first[i].size == second[i].size;
  }
  check(identical, "two calls with identical arguments produce identical results");
}

void testTimeAffectsPositions() {
  ParticleSystem system(kCount, 42);
  std::vector<ParticleInstance> atZero;
  std::vector<ParticleInstance> atTwo;
  system.computeInstances(0.0f, 1.0f, atZero);
  system.computeInstances(2.0f, 1.0f, atTwo);

  bool anyDifferent = false;
  for (size_t i = 0; i < atZero.size(); ++i) {
    if (atZero[i].x != atTwo[i].x || atZero[i].y != atTwo[i].y || atZero[i].alpha != atTwo[i].alpha) {
      anyDifferent = true;
      break;
    }
  }
  check(anyDifferent, "elapsedSeconds actually changes at least some particle instances");
}

void testIntensityZeroGatesAlpha() {
  ParticleSystem system(kCount, 42);
  std::vector<ParticleInstance> instances;
  system.computeInstances(1.5f, 0.0f, instances);

  bool allZeroAlpha = true;
  for (const auto& inst : instances) {
    if (inst.alpha != 0.0f) allZeroAlpha = false;
  }
  check(allZeroAlpha, "intensity=0 zeroes alpha for every particle");
}

}  // namespace

int main() {
  testCountMatchesConstructor();
  testInstancesWithinBounds();
  testDeterminism();
  testTimeAffectsPositions();
  testIntensityZeroGatesAlpha();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
  }
  std::printf("%d test(s) failed.\n", g_failures);
  return EXIT_FAILURE;
}
