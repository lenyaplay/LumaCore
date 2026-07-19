#include "EncoderSession.h"

#ifdef LUMACORE_HAVE_FFMPEG

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
}

// EncoderSession::start() otherwise fails silently on any step (matches
// existing iOS behavior) — on Android that left a first-run encoder failure
// undiagnosable from Dart's generic "native startRecording failed" alone, so
// the Android build logs the specific libavformat/libavcodec error via
// logcat instead.
#if defined(__ANDROID__)
#define LUMACORE_ENC_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "LumaCoreEnc", __VA_ARGS__)
#else
#define LUMACORE_ENC_LOGE(...)
#endif

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

  // submitAudioFrame() runs on the audio-capture thread; stop() runs on
  // whatever thread the caller drives recording lifecycle from (a separate
  // "encode"/recording thread on both iOS and Android) — both read/write
  // audioAccum/audioAccumSamples with no other synchronization. Without
  // this, stop() can observe a torn audioAccumSamples mid-update from a
  // concurrent submitAudioFrame() call, underflowing the final partial-
  // frame's `frameSize*sizeof(float) - haveBytes` (unsigned) computation
  // into a huge value and memset()-ing far past the buffer — reproduced as
  // a SIGSEGV in __memset_aarch64 on-device (ai_plans/04 §8 follow-up).
  std::mutex audioAccumMutex;

#if defined(__ANDROID__)
  // Android-only: submitFrame() otherwise called the FFmpeg encode
  // (avcodec_send_frame -> AMediaCodec round-trip -> avcodec_receive_packet
  // -> mux write) synchronously on the caller's thread, which is the same
  // GL/render thread that drives the live preview (nativeRenderFrame calls
  // straight into RenderPipeline::renderFrame + this). On real MediaCodec
  // hardware that round-trip can run tens of ms, throttling the *entire*
  // camera pipeline — including preview — to encode speed while recording
  // (ai_plans/04 §8 follow-up, reported as visible preview stutter). iOS's
  // VideoToolbox path hasn't shown the same symptom, so this stays
  // Android-only rather than restructuring the shared submitFrame contract.
  struct QueuedVideoFrame {
    std::vector<uint8_t> nv12;  // y plane then uv plane, tightly packed
    size_t yStride = 0;
    size_t uvStride = 0;
    int height = 0;
    int64_t ptsUs = 0;
  };
  // Backpressure cap: if encoding falls behind real-time, drop the oldest
  // queued frame rather than grow memory unboundedly or fall further and
  // further behind — same "drop rather than backlog" principle as
  // RenderPipeline's droppedFrames counter on the preview side.
  static constexpr size_t kMaxQueuedVideoFrames = 4;
  std::thread videoEncodeThread;
  std::mutex videoQueueMutex;
  std::condition_variable videoQueueCv;
  std::deque<QueuedVideoFrame> videoQueue;
  bool videoThreadStop = false;

  // Blocks until every already-queued frame has been encoded and the
  // worker thread has exited. Must run before flushing/closing the video
  // codec (stop()) and before freeing frame/codecCtx (reset(), which the
  // thread reads via encodeNV12FrameSync()) — safe to call twice (stop()
  // then reset()): joinable() is false once already joined.
  void stopVideoThread() {
    {
      std::lock_guard<std::mutex> lock(videoQueueMutex);
      videoThreadStop = true;
    }
    videoQueueCv.notify_all();
    if (videoEncodeThread.joinable()) videoEncodeThread.join();
    videoQueue.clear();
    videoThreadStop = false;
  }
#endif

  void reset() {
#if defined(__ANDROID__)
    stopVideoThread();
#endif
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
  int sendRet = avcodec_send_frame(codecCtx, frame);
  if (sendRet < 0) {
    char errbuf[128];
    av_strerror(sendRet, errbuf, sizeof(errbuf));
    LUMACORE_ENC_LOGE("avcodec_send_frame(stream %d) failed: %s", stream->index, errbuf);
    return false;
  }
  for (;;) {
    int ret = avcodec_receive_packet(codecCtx, packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      char errbuf[128];
      av_strerror(ret, errbuf, sizeof(errbuf));
      LUMACORE_ENC_LOGE("avcodec_receive_packet(stream %d) failed: %s", stream->index, errbuf);
      return false;
    }
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

// Copies one NV12 frame's Y/UV planes into impl.frame, stamps its pts
// relative to the shared firstPtsUs origin, and hands it to the encoder —
// the actual per-row-copy + avcodec_send_frame/receive_packet/mux-write work
// submitFrame() used to do inline. Called synchronously on the caller's
// thread on iOS; called from the dedicated videoEncodeThread on Android (see
// the Impl comment above).
void encodeNV12FrameSync(EncoderSession::Impl& impl, const uint8_t* yPlane, size_t yStride, const uint8_t* uvPlane,
                          size_t uvStride, int height, int64_t ptsUs) {
  if (av_frame_make_writable(impl.frame) < 0) return;

  uint8_t* dstY = impl.frame->data[0];
  size_t dstYStride = static_cast<size_t>(impl.frame->linesize[0]);
  size_t copyYBytes = std::min(yStride, dstYStride);
  for (int row = 0; row < height; ++row) {
    std::memcpy(dstY + row * dstYStride, yPlane + row * yStride, copyYBytes);
  }

  uint8_t* dstUv = impl.frame->data[1];
  size_t dstUvStride = static_cast<size_t>(impl.frame->linesize[1]);
  size_t copyUvBytes = std::min(uvStride, dstUvStride);
  int uvHeight = height / 2;
  for (int row = 0; row < uvHeight; ++row) {
    std::memcpy(dstUv + row * dstUvStride, uvPlane + row * uvStride, copyUvBytes);
  }

  int64_t delta;
  {
    std::lock_guard<std::mutex> lock(impl.muxMutex);
    if (impl.firstPtsUs < 0) impl.firstPtsUs = ptsUs;
    delta = ptsUs - impl.firstPtsUs;
  }
  impl.frame->pts = delta < 0 ? 0 : delta;

  sendAndDrain(impl, impl.codecCtx, impl.stream, impl.packet, impl.frame);
}

#if defined(__ANDROID__)
// Drains impl.videoQueue until told to stop AND the queue is empty (in that
// order — draining fully before exiting is what makes Impl::reset()'s
// join() a correct "wait for all queued frames to finish encoding").
void videoEncodeThreadLoop(EncoderSession::Impl& impl) {
  for (;;) {
    EncoderSession::Impl::QueuedVideoFrame item;
    {
      std::unique_lock<std::mutex> lock(impl.videoQueueMutex);
      impl.videoQueueCv.wait(lock, [&] { return !impl.videoQueue.empty() || impl.videoThreadStop; });
      if (impl.videoQueue.empty()) {
        if (impl.videoThreadStop) return;
        continue;
      }
      item = std::move(impl.videoQueue.front());
      impl.videoQueue.pop_front();
    }
    const uint8_t* yPlane = item.nv12.data();
    const uint8_t* uvPlane = item.nv12.data() + item.yStride * static_cast<size_t>(item.height);
    encodeNV12FrameSync(impl, yPlane, item.yStride, uvPlane, item.uvStride, item.height, item.ptsUs);
  }
}
#endif

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
#if defined(__APPLE__)
  const AVCodec* videoCodec = avcodec_find_encoder_by_name("h264_videotoolbox");
#elif defined(__ANDROID__)
  const AVCodec* videoCodec = avcodec_find_encoder_by_name("h264_mediacodec");
#else
  const AVCodec* videoCodec = nullptr;
#endif
  if (!videoCodec) {
    LUMACORE_ENC_LOGE("no h264_mediacodec encoder registered");
    return false;
  }
  const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!audioCodec) {
    LUMACORE_ENC_LOGE("no aac encoder registered");
    return false;
  }

  int ret = avformat_alloc_output_context2(&impl_->fmtCtx, nullptr, "mp4", outPath.c_str());
  if (ret < 0 || !impl_->fmtCtx) {
    char errbuf[128];
    av_strerror(ret, errbuf, sizeof(errbuf));
    LUMACORE_ENC_LOGE("avformat_alloc_output_context2(%s) failed: %s", outPath.c_str(), errbuf);
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
  ret = avcodec_open2(impl_->codecCtx, videoCodec, nullptr);
  if (ret < 0) {
    char errbuf[128];
    av_strerror(ret, errbuf, sizeof(errbuf));
    LUMACORE_ENC_LOGE("avcodec_open2(h264_mediacodec, %dx%d) failed: %s", width, height, errbuf);
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
  ret = avcodec_open2(impl_->audioCodecCtx, audioCodec, nullptr);
  if (ret < 0) {
    char errbuf[128];
    av_strerror(ret, errbuf, sizeof(errbuf));
    LUMACORE_ENC_LOGE("avcodec_open2(aac) failed: %s", errbuf);
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
    ret = avio_open(&impl_->fmtCtx->pb, outPath.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      char errbuf[128];
      av_strerror(ret, errbuf, sizeof(errbuf));
      LUMACORE_ENC_LOGE("avio_open(%s) failed: %s", outPath.c_str(), errbuf);
      impl_->reset();
      return false;
    }
  }
  ret = avformat_write_header(impl_->fmtCtx, nullptr);
  if (ret < 0) {
    char errbuf[128];
    av_strerror(ret, errbuf, sizeof(errbuf));
    LUMACORE_ENC_LOGE("avformat_write_header failed: %s", errbuf);
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

#if defined(__ANDROID__)
  impl_->videoThreadStop = false;
  impl_->videoEncodeThread = std::thread([this] { videoEncodeThreadLoop(*impl_); });
#endif

  recording_ = true;
  return true;
}

void EncoderSession::submitFrame(void* platformImageHandle, int64_t ptsUs) {
  if (!recording_ || !platformImageHandle) return;

#if defined(__APPLE__)
  // CPU-copy path (Path B, deliberately not a zero-copy AV_PIX_FMT_VIDEOTOOLBOX
  // wrap — see ai_plans/01-ios-ffmpeg-minimal-recording.md §4). Real bytesPerRow
  // per plane is read from the buffer, never assumed to equal width. Encoded
  // synchronously here — see the Impl comment on videoEncodeThread for why
  // Android instead queues a copy for a dedicated thread.
  auto pixelBuffer = static_cast<CVPixelBufferRef>(platformImageHandle);
  if (CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
    return;
  }
  const auto* yPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
  size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
  const auto* uvPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
  size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
  int height = static_cast<int>(CVPixelBufferGetHeightOfPlane(pixelBuffer, 0));
  encodeNV12FrameSync(*impl_, yPlane, yStride, uvPlane, uvStride, height, ptsUs);
  CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
#elif defined(__ANDROID__)
  // Copies GLRenderBackend::exportForEncoder()'s heap-allocated
  // NativeNV12Buffer (which the caller frees right after this call returns —
  // see EncoderSession.h) into a queue entry the dedicated videoEncodeThread
  // owns, so the actual encode (avcodec_send_frame -> MediaCodec round-trip
  // -> avcodec_receive_packet -> mux write) never runs on the caller's
  // thread (the GL/render thread — see the Impl comment above).
  auto* buf = static_cast<const NativeNV12Buffer*>(platformImageHandle);
  size_t ySize = buf->yStride * static_cast<size_t>(buf->height);
  size_t uvSize = buf->uvStride * static_cast<size_t>(buf->height / 2);

  Impl::QueuedVideoFrame item;
  item.nv12.resize(ySize + uvSize);
  std::memcpy(item.nv12.data(), buf->yPlane, ySize);
  std::memcpy(item.nv12.data() + ySize, buf->uvPlane, uvSize);
  item.yStride = buf->yStride;
  item.uvStride = buf->uvStride;
  item.height = buf->height;
  item.ptsUs = ptsUs;

  {
    std::lock_guard<std::mutex> lock(impl_->videoQueueMutex);
    if (impl_->videoQueue.size() >= Impl::kMaxQueuedVideoFrames) {
      impl_->videoQueue.pop_front();
    }
    impl_->videoQueue.push_back(std::move(item));
  }
  impl_->videoQueueCv.notify_one();
#endif
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

  std::lock_guard<std::mutex> accumLock(impl_->audioAccumMutex);

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

  {
    std::lock_guard<std::mutex> accumLock(impl_->audioAccumMutex);
    if (impl_->audioCodecCtx && impl_->audioAccumSamples > 0) {
      int frameSize = impl_->audioCodecCtx->frame_size;
      av_frame_make_writable(impl_->audioFrame);
      size_t haveBytes = impl_->audioAccumSamples * sizeof(float);
      std::memcpy(impl_->audioFrame->data[0], impl_->audioAccum.data(), haveBytes);
      std::memset(impl_->audioFrame->data[0] + haveBytes, 0,
                  static_cast<size_t>(frameSize) * sizeof(float) - haveBytes);
      impl_->audioFrame->nb_samples = frameSize;
      impl_->audioFrame->pts = impl_->audioSampleOffset + impl_->audioSamplesEncoded;
      sendAndDrain(*impl_, impl_->audioCodecCtx, impl_->audioStream, impl_->audioPacket, impl_->audioFrame);
      impl_->audioAccumSamples = 0;
    }
  }

#if defined(__ANDROID__)
  // Must finish encoding every already-queued frame (and stop the worker
  // thread) before the video-flush call below puts the codec into draining
  // state — sending it a real frame afterwards would be invalid.
  impl_->stopVideoThread();
#endif

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
