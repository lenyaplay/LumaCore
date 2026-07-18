#include "PlatformRenderBackendFactory.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// __APPLE__ alone is not enough to select Metal: it is also defined when this
// TU is compiled for the plain macOS host (e.g. the toolchain-less
// lumacore_tests build used for CI/headless verification), where
// MetalRenderBackend.mm is never linked in and instantiating the class would
// fail at link time with an undefined vtable. TARGET_OS_IPHONE narrows this
// to actual iOS device/simulator builds, which is when `lumacore` (the iOS
// CMake target that compiles MetalRenderBackend.mm) is the thing linking
// lumacore_logic in.
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include "metal/MetalRenderBackend.h"
#elif defined(__ANDROID__)
#include "gl/GLRenderBackend.h"
#elif defined(_WIN32)
#include "vulkan/VulkanRenderBackend.h"
#endif

namespace lumacore::render {

std::unique_ptr<IRenderBackend> createPlatformRenderBackend() {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  return std::make_unique<metal::MetalRenderBackend>();
#elif defined(__ANDROID__)
  return std::make_unique<gl::GLRenderBackend>();
#elif defined(_WIN32)
  return std::make_unique<vulkan::VulkanRenderBackend>();
#else
  return nullptr;  // host-only skeleton build (CI, tests without a device)
#endif
}

}  // namespace lumacore::render
