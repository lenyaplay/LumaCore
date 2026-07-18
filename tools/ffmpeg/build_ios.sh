#!/usr/bin/env bash
# Cross-compiles FFmpeg for iOS: device (arm64) + Apple-Silicon-simulator
# (arm64) slices, via xcrun/clang directly (no Xcode project involved).
# See ARCHITECTURE.md §4 and docs/ai_plans/01-ios-ffmpeg-minimal-recording.md.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./common.sh

require_ffmpeg_src

MIN_IOS_VERSION="${MIN_IOS_VERSION:-13.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
REPO_ROOT="$(cd ../.. && pwd)"
OUT_ROOT="${REPO_ROOT}/native/third_party/ffmpeg_build/ios"
mkdir -p "${OUT_ROOT}"

# slice name -> (sdk, extra clang/version flag)
SLICES=(
  "device-arm64:iphoneos:-miphoneos-version-min=${MIN_IOS_VERSION}"
  "sim-arm64:iphonesimulator:-mios-simulator-version-min=${MIN_IOS_VERSION}"
)

build_slice() {
  local name="$1" sdk="$2" version_flag="$3"
  local sysroot cc out_dir build_dir
  sysroot="$(xcrun --sdk "${sdk}" --show-sdk-path)"
  cc="$(xcrun --sdk "${sdk}" -f clang)"
  out_dir="${OUT_ROOT}/${name}"
  build_dir="${FFMPEG_SRC_DIR}/build-ios-${name}"

  echo "=== Building FFmpeg for iOS slice: ${name} (sdk=${sdk}) ===" >&2

  rm -rf "${build_dir}"
  mkdir -p "${build_dir}" "${out_dir}"

  local extra_flags="-arch arm64 -isysroot ${sysroot} ${version_flag}"

  (
    cd "${build_dir}"
    # shellcheck disable=SC2086
    "${FFMPEG_SRC_DIR}/configure" \
      "${FFMPEG_COMMON_FLAGS[@]}" \
      "${FFMPEG_IOS_FLAGS[@]}" \
      --target-os=darwin \
      --arch=arm64 \
      --cpu=armv8 \
      --enable-cross-compile \
      --enable-pic \
      --enable-static \
      --disable-shared \
      --sysroot="${sysroot}" \
      --cc="${cc}" \
      --as="${cc}" \
      --extra-cflags="${extra_flags}" \
      --extra-cxxflags="${extra_flags}" \
      --extra-ldflags="${extra_flags}" \
      --prefix="${out_dir}"

    make -j"${JOBS}"
    make install
  )
}

for entry in "${SLICES[@]}"; do
  IFS=":" read -r name sdk version_flag <<<"${entry}"
  build_slice "${name}" "${sdk}" "${version_flag}"
done

echo "=== FFmpeg iOS build complete: ${OUT_ROOT} ===" >&2
