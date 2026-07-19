#!/usr/bin/env bash
# Cross-compiles FFmpeg for Android via NDK clang (single slice: arm64-v8a —
# the emulator and all current devices are arm64, armeabi-v7a is
# deliberately not built, same arm64-only scope as the iOS build). See
# ARCHITECTURE.md §4 and docs/ai_plans/04-android-camerax-gl-pipeline.md §A.2.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./common.sh

require_ffmpeg_src
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set (path to the Android NDK, e.g. \$ANDROID_HOME/ndk/<version>)}"

MIN_ANDROID_API="${MIN_ANDROID_API:-24}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
REPO_ROOT="$(cd ../.. && pwd)"
OUT_ROOT="${REPO_ROOT}/native/third_party/ffmpeg_build/android"
ABI="arm64-v8a"
OUT_DIR="${OUT_ROOT}/${ABI}"
BUILD_DIR="${FFMPEG_SRC_DIR}/build-android-${ABI}"

case "$(uname -s)" in
  Darwin) HOST_TAG="darwin-x86_64" ;;
  Linux) HOST_TAG="linux-x86_64" ;;
  *) echo "error: unsupported host OS for NDK toolchain lookup" >&2; exit 1 ;;
esac

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
if [[ ! -d "${TOOLCHAIN}" ]]; then
  echo "error: NDK toolchain not found at ${TOOLCHAIN} (check ANDROID_NDK_HOME)" >&2
  exit 1
fi

CC="${TOOLCHAIN}/bin/aarch64-linux-android${MIN_ANDROID_API}-clang"
CXX="${TOOLCHAIN}/bin/aarch64-linux-android${MIN_ANDROID_API}-clang++"
AR="${TOOLCHAIN}/bin/llvm-ar"
NM="${TOOLCHAIN}/bin/llvm-nm"
RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
STRIP="${TOOLCHAIN}/bin/llvm-strip"
SYSROOT="${TOOLCHAIN}/sysroot"

echo "=== Building FFmpeg for Android slice: ${ABI} (API ${MIN_ANDROID_API}) ===" >&2

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}" "${OUT_DIR}"

(
  cd "${BUILD_DIR}"
  # -fvisibility=hidden: without it, linking these static libs into our PIC
  # .so fails with "relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used
  # against symbol 'ff_tx_tab_*_float'; recompile with -fPIC" — libavutil's
  # aarch64 NEON tx (FFT) assembly addresses internal-linkage data (twiddle
  # tables) via adrp/add, which is only valid against non-preemptible
  # (hidden-visibility) symbols in PIC code; FFmpeg's own build doesn't mark
  # them hidden by default. NEON stays enabled (aarch64 has no runtime NEON
  # toggle worth disabling — it's baseline ISA, not optional like on armv7).
  # shellcheck disable=SC2086
  "${FFMPEG_SRC_DIR}/configure" \
    "${FFMPEG_COMMON_FLAGS[@]}" \
    "${FFMPEG_ANDROID_FLAGS[@]}" \
    --target-os=android \
    --arch=aarch64 \
    --cpu=armv8-a \
    --enable-cross-compile \
    --enable-pic \
    --enable-static \
    --disable-shared \
    --sysroot="${SYSROOT}" \
    --cc="${CC}" \
    --cxx="${CXX}" \
    --ar="${AR}" \
    --nm="${NM}" \
    --ranlib="${RANLIB}" \
    --strip="${STRIP}" \
    --extra-cflags="-O2 -fPIC -fvisibility=hidden" \
    --extra-cxxflags="-O2 -fPIC -fvisibility=hidden" \
    --prefix="${OUT_DIR}"

  make -j"${JOBS}"
  make install
)

echo "=== FFmpeg Android build complete: ${OUT_DIR} ===" >&2
