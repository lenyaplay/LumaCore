cmake_minimum_required(VERSION 3.20)
project(LumaCore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- lumacore_logic: pure/platform-agnostic C++ (ARCHITECTURE.md §5) ---------
# Configures and builds on any host with no NDK/ios-cmake/MSVC toolchain — this
# is the part CI can exercise on a plain Linux/macOS runner (Task.md §6, Этап 0).
add_library(lumacore_logic STATIC
  src/api/lumacore_api.cpp
  src/render/RenderPipeline.cpp
  src/encode/EncoderSession.cpp
  src/license/TokenValidator.cpp
)
target_include_directories(lumacore_logic PUBLIC src)

enable_testing()
add_subdirectory(tests)

# --- Platform backends (ARCHITECTURE.md §2) -----------------------------------
# Only added when the corresponding toolchain is actually in use. On a bare
# host configure (no -DCMAKE_TOOLCHAIN_FILE), none of these apply and only
# lumacore_logic + lumacore_tests are built — that is the expected skeleton
# state until the Android NDK / ios-cmake / MSVC+Vulkan toolchains land.

if(ANDROID)
  add_library(lumacore SHARED
    src/render/gl/GLRenderBackend.cpp
    jni/lumacore_jni.cpp
  )
  target_link_libraries(lumacore PRIVATE lumacore_logic)
  target_link_libraries(lumacore PRIVATE GLESv3 EGL log)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  add_library(lumacore STATIC
    src/render/metal/MetalRenderBackend.mm
    objcxx/LumaCoreBridge.mm
  )
  target_link_libraries(lumacore PRIVATE lumacore_logic)
  target_link_libraries(lumacore PRIVATE "-framework Metal" "-framework CoreVideo" "-framework Foundation")
elseif(WIN32)
  add_definitions(-DLUMACORE_EXPORTS)
  add_library(lumacore SHARED
    src/render/vulkan/VulkanRenderBackend.cpp
    src/interop/D3D11VulkanBridge.cpp
    win32/DpapiFingerprint.cpp
    win32/MfWorkQueue.cpp
  )
  target_link_libraries(lumacore PRIVATE lumacore_logic)
  find_package(Vulkan)
  if(Vulkan_FOUND)
    target_link_libraries(lumacore PRIVATE Vulkan::Vulkan)
  else()
    message(WARNING "Vulkan SDK not found (set VULKAN_SDK) — lumacore target will fail to link until it is installed.")
  endif()
  target_link_libraries(lumacore PRIVATE mfplat mfuuid strmiids ole32)
endif()
