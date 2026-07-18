#include "EncoderSession.h"

#ifdef LUMACORE_HAVE_FFMPEG

#include <algorithm>
#include <cstring>

#include <CoreVideo/CoreVideo.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

namespace lumacore::encode {

struct EncoderSession::Impl {
  AVFormatContext* fmtCtx = nullptr;
  AVCodecContext* codecCtx = nullptr;
  AVStream* stream = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  int64_t firstPtsUs = -1;

  void reset() {
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (fmtCtx) {
      if (!(fmtCtx->oformat->flags & AVFMT_NOFILE) && fmtCtx->pb) {
        avio_closep(&fmtCtx->pb);
      }
      avformat_free_context(fmtCtx);
      fmtCtx = nullptr;
    }
    stream = nullptr;
    firstPtsUs = -1;
  }
};

namespace {

// Sends `frame` (nullptr to flush) and drains every packet the encoder has
// ready, muxing each one. Used both for live frames and for the stop()
// flush — the moov atom (av_write_trailer) is only correct once the encoder
// has been fully drained.
bool sendAndDrain(EncoderSession::Impl& impl, AVFrame* frame) {
  if (avcodec_send_frame(impl.codecCtx, frame) < 0) return false;
  for (;;) {
    int ret = avcodec_receive_packet(impl.codecCtx, impl.packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) return false;
    av_packet_rescale_ts(impl.packet, impl.codecCtx->time_base, impl.stream->time_base);
    impl.packet->stream_index = impl.stream->index;
    av_interleaved_write_frame(impl.fmtCtx, impl.packet);
    av_packet_unref(impl.packet);
  }
  return true;
}

}  // namespace

EncoderSession::EncoderSession() : impl_(std::make_unique<Impl>()) {}

EncoderSession::~EncoderSession() {
  if (recording_) stop();
}

EncoderSession::EncoderSession(EncoderSession&&) noexcept = default;
EncoderSession& EncoderSession::operator=(EncoderSession&&) noexcept = default;

bool EncoderSession::start(const std::string& outPath, int bitrateKbps, int width, int height) {
  if (recording_) return false;
  impl_->reset();

  // No software H.264 fallback — libx264 is intentionally excluded (GPL,
  // ARCHITECTURE.md §4). A missing hardware encoder is a hard failure, not a
  // silent degrade.
  const AVCodec* codec = avcodec_find_encoder_by_name("h264_videotoolbox");
  if (!codec) return false;

  if (avformat_alloc_output_context2(&impl_->fmtCtx, nullptr, "mp4", outPath.c_str()) < 0 ||
      !impl_->fmtCtx) {
    impl_->reset();
    return false;
  }

  impl_->stream = avformat_new_stream(impl_->fmtCtx, nullptr);
  if (!impl_->stream) {
    impl_->reset();
    return false;
  }

  impl_->codecCtx = avcodec_alloc_context3(codec);
  if (!impl_->codecCtx) {
    impl_->reset();
    return false;
  }
  impl_->codecCtx->width = width;
  impl_->codecCtx->height = height;
  impl_->codecCtx->pix_fmt = AV_PIX_FMT_NV12;
  impl_->codecCtx->color_range = AVCOL_RANGE_JPEG;
  // Microsecond timebase matches the ptsUs unit submitFrame() receives
  // (CMSampleBufferGetPresentationTimeStamp on the Swift side), avoiding a
  // frame-rate assumption for what is a variable-frame-rate camera feed.
  impl_->codecCtx->time_base = AVRational{1, 1000000};
  impl_->codecCtx->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
  if (impl_->fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
    impl_->codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  if (avcodec_open2(impl_->codecCtx, codec, nullptr) < 0) {
    impl_->reset();
    return false;
  }

  if (avcodec_parameters_from_context(impl_->stream->codecpar, impl_->codecCtx) < 0) {
    impl_->reset();
    return false;
  }
  impl_->stream->time_base = impl_->codecCtx->time_base;

  if (!(impl_->fmtCtx->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&impl_->fmtCtx->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0) {
      impl_->reset();
      return false;
    }
  }

  if (avformat_write_header(impl_->fmtCtx, nullptr) < 0) {
    impl_->reset();
    return false;
  }

  impl_->frame = av_frame_alloc();
  impl_->frame->format = AV_PIX_FMT_NV12;
  impl_->frame->width = width;
  impl_->frame->height = height;
  if (av_frame_get_buffer(impl_->frame, 32) < 0) {
    impl_->reset();
    return false;
  }

  impl_->packet = av_packet_alloc();
  impl_->firstPtsUs = -1;

  recording_ = true;
  return true;
}

void EncoderSession::submitFrame(void* platformImageHandle, int64_t ptsUs) {
  if (!recording_ || !platformImageHandle) return;

  // CPU-copy path (Path B, deliberately not a zero-copy AV_PIX_FMT_VIDEOTOOLBOX
  // wrap — see ai_plans/01-ios-ffmpeg-minimal-recording.md §4). Real bytesPerRow
  // per plane is read from the buffer, never assumed to equal width.
  auto pixelBuffer = static_cast<CVPixelBufferRef>(platformImageHandle);
  if (CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
    return;
  }

  if (av_frame_make_writable(impl_->frame) < 0) {
    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    return;
  }

  for (int plane = 0; plane < 2; ++plane) {
    const auto* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, plane));
    size_t srcStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, plane);
    size_t planeHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, plane);
    uint8_t* dst = impl_->frame->data[plane];
    size_t dstStride = static_cast<size_t>(impl_->frame->linesize[plane]);
    size_t copyBytes = std::min(srcStride, dstStride);
    for (size_t row = 0; row < planeHeight; ++row) {
      std::memcpy(dst + row * dstStride, src + row * srcStride, copyBytes);
    }
  }

  CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

  if (impl_->firstPtsUs < 0) impl_->firstPtsUs = ptsUs;
  impl_->frame->pts = ptsUs - impl_->firstPtsUs;

  sendAndDrain(*impl_, impl_->frame);
}

bool EncoderSession::stop() {
  if (!recording_) return true;
  recording_ = false;

  sendAndDrain(*impl_, nullptr);
  // The moov atom is written here — this must run on every exit path,
  // including ones reached after an upstream error, or the file is unplayable.
  bool ok = impl_->fmtCtx && av_write_trailer(impl_->fmtCtx) >= 0;
  impl_->reset();
  return ok;
}

}  // namespace lumacore::encode

#else  // !LUMACORE_HAVE_FFMPEG

namespace lumacore::encode {

struct EncoderSession::Impl {};

EncoderSession::EncoderSession() : impl_(std::make_unique<Impl>()) {}
EncoderSession::~EncoderSession() = default;
EncoderSession::EncoderSession(EncoderSession&&) noexcept = default;
EncoderSession& EncoderSession::operator=(EncoderSession&&) noexcept = default;

bool EncoderSession::start(const std::string&, int, int, int) {
  recording_ = true;
  return recording_;
}

void EncoderSession::submitFrame(void*, int64_t) {}

bool EncoderSession::stop() {
  recording_ = false;
  return true;
}

}  // namespace lumacore::encode

#endif  // LUMACORE_HAVE_FFMPEG
