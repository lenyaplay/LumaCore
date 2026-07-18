#!/usr/bin/env bash
# Cross-compiles FFmpeg for iOS (device + Apple-Silicon-simulator arm64 slices)
# via xcodebuild/clang. See ARCHITECTURE.md §4. Stub — Этап 3.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./common.sh

# TODO(Этап 3):
#   - Two slices: device (arm64, --target-os=darwin --sysroot=iphoneos)
#     and simulator (arm64, --sysroot=iphonesimulator).
#   - --enable-videotoolbox, frameworks VideoToolbox/CoreMedia/CoreVideo.
#   - lipo/xcodebuild -create-xcframework into native/third_party/ffmpeg_build/ios/.
#   - Reference process: kewlbear/FFmpeg-iOS-build-script (template only, config is ours).

require_ffmpeg_src

echo "TODO(Этап 3): iOS FFmpeg cross-compile not implemented yet." >&2
exit 1
