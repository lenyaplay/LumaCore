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
  src/render/PlatformRenderBackendFactory.cpp
  src/render/particles/ParticleSystem.cpp
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

  # --- FFmpeg write-path statics (VideoToolbox H.264 encode, mp4 mux) --------
  # Built by tools/ffmpeg/build_ios.sh into one slice dir per PLATFORM (set by
  # native/cmake/ios.toolchain.cmake). See docs/ai_plans/01-ios-ffmpeg-minimal-recording.md.
  if(PLATFORM STREQUAL "OS64")
    set(LUMACORE_FFMPEG_IOS_SLICE "device-arm64")
  elseif(PLATFORM STREQUAL "SIMULATORARM64")
    set(LUMACORE_FFMPEG_IOS_SLICE "sim-arm64")
  else()
    message(FATAL_ERROR "Unsupported iOS PLATFORM '${PLATFORM}' — only OS64 (device) and SIMULATORARM64 (simulator) are built (arm64-only scope, see ai_plans/01-ios-ffmpeg-minimal-recording.md).")
  endif()
  set(LUMACORE_FFMPEG_IOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg_build/ios/${LUMACORE_FFMPEG_IOS_SLICE}")
  if(NOT EXISTS "${LUMACORE_FFMPEG_IOS_DIR}/lib/libavcodec.a")
    message(FATAL_ERROR "FFmpeg iOS build not found at ${LUMACORE_FFMPEG_IOS_DIR} — run tools/ffmpeg/build_ios.sh first.")
  endif()

  target_include_directories(lumacore_logic PRIVATE "${LUMACORE_FFMPEG_IOS_DIR}/include")
  target_link_libraries(lumacore_logic PRIVATE
    "${LUMACORE_FFMPEG_IOS_DIR}/lib/libavformat.a"
    "${LUMACORE_FFMPEG_IOS_DIR}/lib/libavcodec.a"
    "${LUMACORE_FFMPEG_IOS_DIR}/lib/libswscale.a"
    "${LUMACORE_FFMPEG_IOS_DIR}/lib/libswresample.a"
    "${LUMACORE_FFMPEG_IOS_DIR}/lib/libavutil.a"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework AudioToolbox"
    "-liconv"
    "-lz"
  )
  target_compile_definitions(lumacore_logic PRIVATE LUMACORE_HAVE_FFMPEG=1)

  # --- Metal shader library (ai_plans/03-ios-metal-render-pipeline.md §4) ----
  # Per-slice: device and simulator Metal IR are not fat-lib-compatible, same
  # reason the FFmpeg statics above are per-slice. Each .metal file is
  # compiled to its own .air (xcrun metal -c does not accept multiple inputs
  # with a single -o — verified empirically), then metallib links them into
  # one library. Shared helpers in Common.metal must stay non-`inline` or
  # metallib's cross-.air linking silently fails to resolve them.
  if(PLATFORM STREQUAL "OS64")
    set(LUMACORE_METAL_SDK "iphoneos")
  else()
    set(LUMACORE_METAL_SDK "iphonesimulator")
  endif()
  set(LUMACORE_METAL_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/render/metal/shaders")
  set(LUMACORE_METAL_SOURCES
    "${LUMACORE_METAL_SHADER_DIR}/Common.metal"
    "${LUMACORE_METAL_SHADER_DIR}/ColorCorrection.metal"
    "${LUMACORE_METAL_SHADER_DIR}/Vignette.metal"
    "${LUMACORE_METAL_SHADER_DIR}/Particles.metal"
  )
  set(LUMACORE_METAL_HEADERS
    "${LUMACORE_METAL_SHADER_DIR}/Common.h"
    "${LUMACORE_METAL_SHADER_DIR}/ShaderTypes.h"
  )
  set(LUMACORE_METAL_AIRS "")
  foreach(LUMACORE_METAL_SRC ${LUMACORE_METAL_SOURCES})
    get_filename_component(LUMACORE_METAL_NAME "${LUMACORE_METAL_SRC}" NAME_WE)
    set(LUMACORE_METAL_AIR "${CMAKE_CURRENT_BINARY_DIR}/${LUMACORE_METAL_NAME}.air")
    add_custom_command(
      OUTPUT "${LUMACORE_METAL_AIR}"
      COMMAND xcrun -sdk ${LUMACORE_METAL_SDK} metal -c "${LUMACORE_METAL_SRC}"
              -I "${LUMACORE_METAL_SHADER_DIR}" -o "${LUMACORE_METAL_AIR}"
      DEPENDS "${LUMACORE_METAL_SRC}" ${LUMACORE_METAL_HEADERS}
      COMMENT "Compiling Metal shader ${LUMACORE_METAL_NAME} (${LUMACORE_METAL_SDK})"
    )
    list(APPEND LUMACORE_METAL_AIRS "${LUMACORE_METAL_AIR}")
  endforeach()
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lumacore.metallib"
    COMMAND xcrun -sdk ${LUMACORE_METAL_SDK} metallib ${LUMACORE_METAL_AIRS} -o "${CMAKE_CURRENT_BINARY_DIR}/lumacore.metallib"
    DEPENDS ${LUMACORE_METAL_AIRS}
    COMMENT "Linking lumacore.metallib (${LUMACORE_METAL_SDK})"
  )
  add_custom_target(lumacore_metallib DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/lumacore.metallib")
  add_dependencies(lumacore lumacore_metallib)
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
