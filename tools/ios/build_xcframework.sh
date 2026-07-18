#!/usr/bin/env bash
# Rebuilds native/build-ios-{device,sim} from scratch, merges each slice's
# static libs (liblumacore + liblumacore_logic + FFmpeg statics) via
# `libtool -static`, then re-creates ios/LumaCoreKit.xcframework.
#
# This fixes a sequence that, until this script existed, only ever ran ad-hoc
# in a terminal across past sessions (CMake configure/build device+sim ->
# libtool merge -> xcodebuild -create-xcframework) — see
# docs/ai_plans/03-ios-metal-render-pipeline.md §4. Without this script there
# was no way to reproduce an xcframework rebuild from a clean checkout.
#
# Requires: tools/ffmpeg/build_ios.sh already run once (FFmpeg iOS statics
# present under native/third_party/ffmpeg_build/ios/{device,sim}-arm64).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
REPO_ROOT="$(pwd)"
NATIVE_DIR="${REPO_ROOT}/native"
TOOLCHAIN="${NATIVE_DIR}/cmake/ios.toolchain.cmake"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
# Matches Runner.xcodeproj's IPHONEOS_DEPLOYMENT_TARGET — Metal 2.3 (64-bit
# `long` in ShaderTypes.h) needs iOS 14+.
DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-14.0}"

FFMPEG_DEVICE_DIR="${NATIVE_DIR}/third_party/ffmpeg_build/ios/device-arm64"
FFMPEG_SIM_DIR="${NATIVE_DIR}/third_party/ffmpeg_build/ios/sim-arm64"
if [ ! -f "${FFMPEG_DEVICE_DIR}/lib/libavcodec.a" ] || [ ! -f "${FFMPEG_SIM_DIR}/lib/libavcodec.a" ]; then
  echo "error: FFmpeg iOS statics not found — run tools/ffmpeg/build_ios.sh first." >&2
  exit 1
fi

# libtool input order: our own statics first, then FFmpeg (avformat depends
# on avcodec/avutil, so order doesn't matter to libtool -static, but keeping
# a consistent order makes the merged archive's symbol table diff-friendly).
FFMPEG_LIBS=(libavformat.a libavcodec.a libavfilter.a libavdevice.a libswscale.a libswresample.a libavutil.a)

# slice name -> CMake PLATFORM -> ffmpeg slice dir
SLICES=(
  "device:OS64:${FFMPEG_DEVICE_DIR}"
  "sim:SIMULATORARM64:${FFMPEG_SIM_DIR}"
)

build_slice() {
  local name="$1" platform="$2" ffmpeg_dir="$3"
  local build_dir="${NATIVE_DIR}/build-ios-${name}"

  echo "=== [${name}] Configuring+building CMake (PLATFORM=${platform}) ===" >&2
  rm -rf "${build_dir}"
  cmake -S "${NATIVE_DIR}" -B "${build_dir}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DPLATFORM="${platform}" \
    -DDEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}"
  cmake --build "${build_dir}" --target lumacore -j"${JOBS}"

  if [ ! -f "${build_dir}/lumacore.metallib" ]; then
    echo "error: ${build_dir}/lumacore.metallib was not produced — the Xcode Run Script phase would copy a stale/missing file. Aborting before touching Runner.xcodeproj." >&2
    exit 1
  fi

  echo "=== [${name}] Merging static libs (liblumacore + liblumacore_logic + FFmpeg) ===" >&2
  local merged_dir="${build_dir}/merged"
  mkdir -p "${merged_dir}"
  local inputs=("${build_dir}/liblumacore.a" "${build_dir}/liblumacore_logic.a")
  local lib
  for lib in "${FFMPEG_LIBS[@]}"; do
    inputs+=("${ffmpeg_dir}/lib/${lib}")
  done
  libtool -static -o "${merged_dir}/liblumacore_merged.a" "${inputs[@]}"
}

for entry in "${SLICES[@]}"; do
  IFS=":" read -r name platform ffmpeg_dir <<<"${entry}"
  build_slice "${name}" "${platform}" "${ffmpeg_dir}"
done

echo "=== Creating ios/LumaCoreKit.xcframework ===" >&2
XCFRAMEWORK_PATH="${REPO_ROOT}/ios/LumaCoreKit.xcframework"
rm -rf "${XCFRAMEWORK_PATH}"
xcodebuild -create-xcframework \
  -library "${NATIVE_DIR}/build-ios-device/merged/liblumacore_merged.a" \
  -library "${NATIVE_DIR}/build-ios-sim/merged/liblumacore_merged.a" \
  -output "${XCFRAMEWORK_PATH}"

echo "=== Done: ${XCFRAMEWORK_PATH} ===" >&2
echo "Reminder: lumacore.metallib is NOT inside the xcframework — it's copied" >&2
echo "into the app bundle by Runner's own Run Script build phase at Xcode" >&2
echo "build time, per slice (device/sim), from native/build-ios-{device,sim}." >&2
