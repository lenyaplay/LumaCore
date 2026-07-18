#!/usr/bin/env bash
# Shared FFmpeg ./configure flags for all three platform build scripts.
# See ARCHITECTURE.md §4 for the reasoning behind each flag — minimal
# LGPL-clean build, no libx264/GPL, hardware encoders only, write-path only.
set -euo pipefail

FFMPEG_SRC_DIR="${FFMPEG_SRC_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../native/third_party/ffmpeg" && pwd)}"

FFMPEG_COMMON_FLAGS=(
  --disable-everything
  --disable-doc
  --disable-programs
  --disable-network
  --disable-filters
  --disable-bsfs
  --disable-hwaccels
  --enable-avcodec
  --enable-avformat
  --enable-swscale
  --enable-protocol=file
  --enable-muxer=mp4
  --enable-encoder=aac
)

require_ffmpeg_src() {
  if [[ ! -f "${FFMPEG_SRC_DIR}/configure" ]]; then
    echo "error: FFmpeg source not found at ${FFMPEG_SRC_DIR}" >&2
    echo "       native/third_party/ffmpeg/ is a placeholder — vendoring the pinned submodule is Этап 3 work (see its README.md)." >&2
    exit 1
  fi
}
