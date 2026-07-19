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
  --enable-swresample
  --enable-protocol=file
  --enable-muxer=mp4
  --enable-encoder=aac
)

# iOS-specific flags (VideoToolbox hardware H.264 encode, write-path only —
# decode/hwaccel intentionally not enabled, see ARCHITECTURE.md §4).
FFMPEG_IOS_FLAGS=(
  --enable-videotoolbox
  --enable-encoder=h264_videotoolbox
  --extra-ldflags="-framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation -framework AudioToolbox"
)

# Android-specific flags (MediaCodec hardware H.264 encode, write-path only).
# CPU NV12 buffer in, same as h264_videotoolbox on iOS — not the
# Surface/zero-copy MediaCodec input path (see
# docs/ai_plans/04-android-camerax-gl-pipeline.md §A.2: that's an explicit
# fast-follow, not this round).
#
# --enable-bsf=h264_metadata,extract_extradata: libavcodec/mediacodecenc.c's
# mediacodec_init_bsf() reaches for these at avcodec_open2() time whenever
# AV_CODEC_FLAG_GLOBAL_HEADER is set (always true for our mp4 muxer) —
# h264_metadata signals the true display crop when MediaCodec pads
# width/height up to a macroblock-aligned size internally (e.g. a 1080x1080
# request becomes 1088x1088 internally, since 1080 isn't a multiple of 16);
# extract_extradata pulls the SPS/PPS out for the container's avcC box.
# --disable-bsfs above (shared with iOS/Windows) would otherwise make
# avcodec_open2 fail outright with "Bitstream filter not found" — verified
# on-device; iOS's h264_videotoolbox has no equivalent requirement.
FFMPEG_ANDROID_FLAGS=(
  --enable-mediacodec
  --enable-jni
  --enable-encoder=h264_mediacodec
  --enable-bsf=h264_metadata,extract_extradata
)

require_ffmpeg_src() {
  if [[ ! -f "${FFMPEG_SRC_DIR}/configure" ]]; then
    echo "error: FFmpeg source not found at ${FFMPEG_SRC_DIR}" >&2
    echo "       expected the vendored FFmpeg checkout at native/third_party/ffmpeg/ (already used for the iOS build) — is the submodule/checkout missing?" >&2
    exit 1
  fi
}
