#include "EncoderSession.h"

namespace lumacore::encode {

bool EncoderSession::start(const std::string&, int, int, int) {
  // TODO(Этап 3/5): avformat_alloc_output_context2(mp4) + avcodec encoder
  // (h264_mediacodec / h264_videotoolbox / h264_mf) + aac audio encoder.
  recording_ = true;
  return recording_;
}

void EncoderSession::submitFrame(void*, int64_t) {
  // TODO: avcodec_send_frame / receive_packet, muxer write, variable-frame-rate
  // PTS handling on dropped frames (ARCHITECTURE.md §1).
}

bool EncoderSession::stop() {
  recording_ = false;
  return true;
}

}  // namespace lumacore::encode
