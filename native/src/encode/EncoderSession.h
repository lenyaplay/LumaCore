#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace lumacore::encode {

// Wraps avcodec/avformat mux + hardware encoder glue (mediacodec/videotoolbox/mf).
// One session per recording; start/stop mirror lumacore_start_recording /
// lumacore_stop_recording. See ARCHITECTURE.md §4.
//
// Pimpl'd so this header stays FFmpeg-free — it is included from
// lumacore_api.cpp, which also builds in the host-only skeleton configuration
// where no FFmpeg toolchain is available at all (see CMakeLists.txt).
// LUMACORE_HAVE_FFMPEG (set only for the iOS toolchain build, for now) gates
// the real implementation in EncoderSession.cpp; without it, start()/stop()
// keep the original no-op stub behavior.
class EncoderSession {
 public:
  EncoderSession();
  ~EncoderSession();
  EncoderSession(EncoderSession&&) noexcept;
  EncoderSession& operator=(EncoderSession&&) noexcept;
  EncoderSession(const EncoderSession&) = delete;
  EncoderSession& operator=(const EncoderSession&) = delete;

  bool start(const std::string& outPath, int bitrateKbps, int width, int height);
  void submitFrame(void* platformImageHandle, int64_t ptsUs);
  // Encodes a chunk of interleaved signed-16-bit PCM audio into the shared
  // AAC stream. ptsUs is on the same absolute clock as submitFrame's ptsUs —
  // the two streams share one PTS origin (see EncoderSession.cpp). No-op if
  // start() hasn't been called or pcmData is null.
  void submitAudioFrame(const void* pcmData, int numFrames, int sampleRate, int numChannels, int64_t ptsUs);
  bool stop();

  // Public only so EncoderSession.cpp's free helper functions can name it —
  // the definition lives entirely in the .cpp (FFmpeg-free header).
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  bool recording_ = false;
};

}  // namespace lumacore::encode
