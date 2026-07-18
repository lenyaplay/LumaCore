#include "EncoderSession.h"

#ifdef LUMACORE_HAVE_FFMPEG

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <CoreVideo/CoreVideo.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
}

namespace lumacore::encode {

namespace {
// AAC stream constants. Duplicated by hand in
// ios/Runner/CameraCaptureController.swift's AVCaptureAudioDataOutput
// audioSettings — no shared source of truth across Swift/C++, same accepted
// pattern as ShaderTypes.h/EffectParams.h elsewhere in this codebase.
constexpr int kAudioSampleRate = 44100;
constexpr int kAudioChannels = 1;
constexpr int kAudioBitrateKbps = 128;
}  // namespace

struct EncoderSession::Impl {
  AVFormatContext* fmtCtx = nullptr;

  // Video (h264_videotoolbox).
  AVCodecContext* codecCtx = nullptr;
  AVStream* stream = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;

  // Audio (native FFmpeg "aac" encoder).
  AVCodecContext* audioCodecCtx = nullptr;
  AVStream* audioStream = nullptr;
  AVFrame* audioFrame = nullptr;  // FLTP, exactly audioCodecCtx->frame_size samples
  AVPacket* audioPacket = nullptr;
  SwrContext* swr = nullptr;
  int swrInSampleRate = 0;
  int swrInChannels = 0;
  std::vector<uint8_t> audioAccum;  // FLTP mono samples, as raw bytes
  size_t audioAccumSamples = 0;
  int64_t audioSampleOffset = 0;  // audio timeline origin (in samples), relative to firstPtsUs
  int64_t audioSamplesEncoded = 0;
  bool audioAnchored = false;

  // Two OS threads (video capture queue, audio capture queue) call into this
  // shared AVFormatContext/firstPtsUs once audio lands — av_interleaved_write_frame
  // is not safe to call concurrently on the same AVFormatContext, and
  // firstPtsUs's read-modify-write is a plain data race without this.
  std::mutex muxMutex;
  int64_t firstPtsUs = -1;

  void reset() {
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (audioFrame) av_frame_free(&audioFrame);
    if (audioPacket) av_packet_free(&audioPacket);
    if (audioCodecCtx) avcodec_free_context(&audioCodecCtx);
    if (swr) swr_free(&swr);
    if (fmtCtx) {
      if (!(fmtCtx->oformat->flags & AVFMT_NOFILE) && fmtCtx->pb) {
        avio_closep(&fmtCtx->pb);
      }
      avformat_free_context(fmtCtx);
      fmtCtx = nullptr;
    }
    stream = nullptr;
    audioStream = nullptr;
    swrInSampleRate = 0;
    swrInChannels = 0;
    audioAccum.clear();
    audioAccumSamples = 0;
    audioSampleOffset = 0;
    audioSamplesEncoded = 0;
    audioAnchored = false;
    firstPtsUs = -1;
  }
};

namespace {

// Sends `frame` (nullptr to flush) and drains every packet the encoder has
// ready, muxing each one onto `stream`. Used for both video and audio — the
// two streams share one AVFormatContext, serialized by impl.muxMutex since
// video/audio frames can arrive from different capture-callback threads.
bool sendAndDrain(EncoderSession::Impl& impl, AVCodecContext* codecCtx, AVStream* stream, AVPacket* packet,
                   AVFrame* frame) {
  if (avcodec_send_frame(codecCtx, frame) < 0) return false;
  for (;;) {
    int ret = avcodec_receive_packet(codecCtx, packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) return false;
    av_packet_rescale_ts(packet, codecCtx->time_base, stream->time_base);
    packet->stream_index = stream->index;
    {
      std::lock_guard<std::mutex> lock(impl.muxMutex);
      av_interleaved_write_frame(impl.fmtCtx, packet);
    }
    av_packet_unref(packet);
  }
  return true;
}

bool ensureAudioSwr(EncoderSession::Impl& impl, int inSampleRate, int inChannels) {
  if (impl.swr && impl.swrInSampleRate == inSampleRate && impl.swrInChannels == inChannels) return true;
  if (impl.swr) swr_free(&impl.swr);

  AVChannelLayout inLayout;
  av_channel_layout_default(&inLayout, inChannels);
  int ret = swr_alloc_set_opts2(&impl.swr, &impl.audioCodecCtx->ch_layout, impl.audioCodecCtx->sample_fmt,
                                 impl.audioCodecCtx->sample_rate, &inLayout, AV_SAMPLE_FMT_S16, inSampleRate, 0,
                                 nullptr);
  av_channel_layout_uninit(&inLayout);
  if (ret < 0 || !impl.swr || swr_init(impl.swr) < 0) return false;

  impl.swrInSampleRate = inSampleRate;
  impl.swrInChannels = inChannels;
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

  // No software H.264/AAC fallback — libx264/libfdk_aac are intentionally
  // excluded (GPL, ARCHITECTURE.md §4). A missing hardware/native encoder is
  // a hard failure, not a silent degrade.
  const AVCodec* videoCodec = avcodec_find_encoder_by_name("h264_videotoolbox");
  if (!videoCodec) return false;
  const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!audioCodec) return false;

  if (avformat_alloc_output_context2(&impl_->fmtCtx, nullptr, "mp4", outPath.c_str()) < 0 || !impl_->fmtCtx) {
    impl_->reset();
    return false;
  }

  // --- video stream ---
  impl_->stream = avformat_new_stream(impl_->fmtCtx, nullptr);
  if (!impl_->stream) {
    impl_->reset();
    return false;
  }
  impl_->codecCtx = avcodec_alloc_context3(videoCodec);
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
  if (avcodec_open2(impl_->codecCtx, videoCodec, nullptr) < 0) {
    impl_->reset();
    return false;
  }
  if (avcodec_parameters_from_context(impl_->stream->codecpar, impl_->codecCtx) < 0) {
    impl_->reset();
    return false;
  }
  impl_->stream->time_base = impl_->codecCtx->time_base;

  // --- audio stream ---
  impl_->audioStream = avformat_new_stream(impl_->fmtCtx, nullptr);
  if (!impl_->audioStream) {
    impl_->reset();
    return false;
  }
  impl_->audioCodecCtx = avcodec_alloc_context3(audioCodec);
  if (!impl_->audioCodecCtx) {
    impl_->reset();
    return false;
  }
  // The native FFmpeg "aac" encoder only ever supports FLTP — avoids the
  // deprecated AVCodec::sample_fmts field (see avcodec_get_supported_config()
  // for codecs where the supported format set actually varies).
  impl_->audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
  av_channel_layout_default(&impl_->audioCodecCtx->ch_layout, kAudioChannels);
  impl_->audioCodecCtx->sample_rate = kAudioSampleRate;
  impl_->audioCodecCtx->bit_rate = static_cast<int64_t>(kAudioBitrateKbps) * 1000;
  impl_->audioCodecCtx->time_base = AVRational{1, kAudioSampleRate};
  if (impl_->fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
    impl_->audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  if (avcodec_open2(impl_->audioCodecCtx, audioCodec, nullptr) < 0) {
    impl_->reset();
    return false;
  }
  if (avcodec_parameters_from_context(impl_->audioStream->codecpar, impl_->audioCodecCtx) < 0) {
    impl_->reset();
    return false;
  }
  impl_->audioStream->time_base = impl_->audioCodecCtx->time_base;

  // Both streams must exist before the header is written — MP4 records
  // per-track boxes at header-write time, not as tracks are first used.
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

  impl_->audioFrame = av_frame_alloc();
  impl_->audioFrame->format = impl_->audioCodecCtx->sample_fmt;
  av_channel_layout_copy(&impl_->audioFrame->ch_layout, &impl_->audioCodecCtx->ch_layout);
  impl_->audioFrame->sample_rate = impl_->audioCodecCtx->sample_rate;
  impl_->audioFrame->nb_samples = impl_->audioCodecCtx->frame_size;
  if (av_frame_get_buffer(impl_->audioFrame, 0) < 0) {
    impl_->reset();
    return false;
  }
  impl_->audioPacket = av_packet_alloc();

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

  int64_t delta;
  {
    std::lock_guard<std::mutex> lock(impl_->muxMutex);
    if (impl_->firstPtsUs < 0) impl_->firstPtsUs = ptsUs;
    delta = ptsUs - impl_->firstPtsUs;
  }
  impl_->frame->pts = delta < 0 ? 0 : delta;

  sendAndDrain(*impl_, impl_->codecCtx, impl_->stream, impl_->packet, impl_->frame);
}

void EncoderSession::submitAudioFrame(const void* pcmData, int numFrames, int sampleRate, int numChannels,
                                       int64_t ptsUs) {
  if (!recording_ || !pcmData || numFrames <= 0) return;
  if (!ensureAudioSwr(*impl_, sampleRate, numChannels)) return;

  {
    std::lock_guard<std::mutex> lock(impl_->muxMutex);
    if (impl_->firstPtsUs < 0) impl_->firstPtsUs = ptsUs;
    if (!impl_->audioAnchored) {
      // Anchor audio's sample-0 to the shared origin. Audio callbacks are
      // inherently buffered (~23ms at 1024 samples/44.1kHz), so it's expected
      // — not just defensive — for this stream's first delivered chunk to
      // report a ptsUs at or after firstPtsUs; clamp instead of going negative.
      int64_t deltaUs = ptsUs - impl_->firstPtsUs;
      if (deltaUs < 0) deltaUs = 0;
      impl_->audioSampleOffset = av_rescale(deltaUs, impl_->audioCodecCtx->sample_rate, 1000000);
      impl_->audioAnchored = true;
    }
  }

  const uint8_t* inData[1] = {static_cast<const uint8_t*>(pcmData)};
  int maxOutSamples = swr_get_out_samples(impl_->swr, numFrames);
  if (maxOutSamples < 0) return;

  size_t priorSamples = impl_->audioAccumSamples;
  impl_->audioAccum.resize((priorSamples + static_cast<size_t>(maxOutSamples)) * sizeof(float));
  uint8_t* outData[1] = {impl_->audioAccum.data() + priorSamples * sizeof(float)};
  int converted = swr_convert(impl_->swr, outData, maxOutSamples, inData, numFrames);
  if (converted < 0) return;
  impl_->audioAccumSamples += static_cast<size_t>(converted);

  const int frameSize = impl_->audioCodecCtx->frame_size;  // 1024 for the native aac encoder
  size_t offset = 0;
  while (impl_->audioAccumSamples - offset >= static_cast<size_t>(frameSize)) {
    av_frame_make_writable(impl_->audioFrame);
    std::memcpy(impl_->audioFrame->data[0], impl_->audioAccum.data() + offset * sizeof(float),
                static_cast<size_t>(frameSize) * sizeof(float));
    impl_->audioFrame->nb_samples = frameSize;
    impl_->audioFrame->pts = impl_->audioSampleOffset + impl_->audioSamplesEncoded;
    impl_->audioSamplesEncoded += frameSize;
    sendAndDrain(*impl_, impl_->audioCodecCtx, impl_->audioStream, impl_->audioPacket, impl_->audioFrame);
    offset += static_cast<size_t>(frameSize);
  }

  size_t leftover = impl_->audioAccumSamples - offset;
  std::memmove(impl_->audioAccum.data(), impl_->audioAccum.data() + offset * sizeof(float), leftover * sizeof(float));
  impl_->audioAccumSamples = leftover;
}

bool EncoderSession::stop() {
  if (!recording_) return true;
  recording_ = false;

  if (impl_->audioCodecCtx && impl_->audioAccumSamples > 0) {
    int frameSize = impl_->audioCodecCtx->frame_size;
    av_frame_make_writable(impl_->audioFrame);
    size_t haveBytes = impl_->audioAccumSamples * sizeof(float);
    std::memcpy(impl_->audioFrame->data[0], impl_->audioAccum.data(), haveBytes);
    std::memset(impl_->audioFrame->data[0] + haveBytes, 0, static_cast<size_t>(frameSize) * sizeof(float) - haveBytes);
    impl_->audioFrame->nb_samples = frameSize;
    impl_->audioFrame->pts = impl_->audioSampleOffset + impl_->audioSamplesEncoded;
    sendAndDrain(*impl_, impl_->audioCodecCtx, impl_->audioStream, impl_->audioPacket, impl_->audioFrame);
    impl_->audioAccumSamples = 0;
  }

  // The moov atom is written here — this must run on every exit path,
  // including ones reached after an upstream error, or the file is unplayable.
  sendAndDrain(*impl_, impl_->codecCtx, impl_->stream, impl_->packet, nullptr);        // flush video
  sendAndDrain(*impl_, impl_->audioCodecCtx, impl_->audioStream, impl_->audioPacket, nullptr);  // flush audio
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

void EncoderSession::submitAudioFrame(const void*, int, int, int, int64_t) {}

bool EncoderSession::stop() {
  recording_ = false;
  return true;
}

}  // namespace lumacore::encode

#endif  // LUMACORE_HAVE_FFMPEG
