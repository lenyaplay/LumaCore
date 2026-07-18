#pragma once

#include <memory>

#include "IRenderBackend.h"

namespace lumacore::render {

// Selects the platform IRenderBackend at compile time. Lives in
// lumacore_logic (platform-agnostic target, builds on a bare host with no
// toolchain) so lumacore_api.cpp never #includes a platform-specific backend
// header directly. See ai_plans/03-ios-metal-render-pipeline.md §6.
std::unique_ptr<IRenderBackend> createPlatformRenderBackend();

}  // namespace lumacore::render
