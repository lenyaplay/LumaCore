#!/usr/bin/env bash
# Cross-compiles FFmpeg for Android via NDK clang. See ARCHITECTURE.md §4.
# Stub — Этап 3. Requires ANDROID_NDK_HOME and the FFmpeg submodule to be vendored.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./common.sh

# TODO(Этап 3):
#   - Default ABI: arm64-v8a (--arch=aarch64 --cpu=armv8-a), optional armeabi-v7a.
#   - Toolchain via $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/<host-tag>/bin.
#   - --enable-mediacodec --enable-jni --enable-cross-compile
#     --target-os=android --cc=<ndk-clang> --enable-pic.
#   - Output static libs into native/third_party/ffmpeg_build/android/<abi>/.

require_ffmpeg_src
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set (path to the Android NDK)}"

echo "TODO(Этап 3): Android FFmpeg cross-compile not implemented yet." >&2
exit 1
