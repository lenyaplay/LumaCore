#pragma once

#include <cstdint>
#include <string>

namespace lumacore::encode {

// Wraps avcodec/avformat mux + hardware encoder glue (mediacodec/videotoolbox/mf).
// One session per recording; start/stop mirror lumacore_start_recording /
// lumacore_stop_recording. See ARCHITECTURE.md §4. Stub — implemented in Этап 3/5.
class EncoderSession {
 public:
  bool start(const std::string& outPath, int bitrateKbps, int width, int height);
  void submitFrame(void* platformImageHandle, int64_t ptsUs);
  bool stop();

 private:
  bool recording_ = false;
};

}  // namespace lumacore::encode
