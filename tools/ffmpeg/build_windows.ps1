# Cross/native-compiles FFmpeg for Windows under MSYS2 with --toolchain=msvc
# (cl.exe as the actual compiler — MSYS2 only supplies the unix-like shell for
# ./configure/make). See ARCHITECTURE.md §4. Stub — Этап 10.

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$FfmpegSrcDir = Join-Path $ScriptDir "..\..\native\third_party\ffmpeg"

if (-not (Test-Path (Join-Path $FfmpegSrcDir "configure"))) {
    Write-Error "FFmpeg source not found at $FfmpegSrcDir — native/third_party/ffmpeg/ is a placeholder, vendoring the pinned submodule is Этап 3 work."
    exit 1
}

# TODO(Этап 10):
#   - Run under MSYS2 shell with --toolchain=msvc (not MinGW — ABI must match
#     the MSVC-built native/CMakeLists.txt Windows target).
#   - --enable-mediafoundation, link mfplat/mfuuid/strmiids/ole32 (h264_mf encoder).
#   - Output static libs into native/third_party/ffmpeg_build/windows/x64/.

Write-Error "TODO(Этап 10): Windows FFmpeg build not implemented yet."
exit 1
