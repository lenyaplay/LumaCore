#ifdef __APPLE__

#import "LumaCoreBridge.h"

#import <AudioToolbox/AudioToolbox.h>

#include "api/lumacore_api.h"

@implementation LumaCoreBridge

- (int64_t)renderInitWithContext:(void*)ctx width:(int)width height:(int)height {
  return lumacore_render_init(ctx, width, height);
}

- (void)release:(int64_t)session {
  lumacore_release(session);
}

- (BOOL)startRecording:(int64_t)session
             outputPath:(NSString*)outputPath
            bitrateKbps:(int)bitrateKbps
                  width:(int)width
                 height:(int)height {
  return lumacore_start_recording(session, outputPath.UTF8String, bitrateKbps, width, height) == 0;
}

- (nullable CVPixelBufferRef)renderFrame:(int64_t)session
                              pixelBuffer:(CVPixelBufferRef)pixelBuffer
                                    ptsUs:(int64_t)ptsUs {
  void* outPreviewImage = NULL;
  lumacore_render_frame(session, (void*)pixelBuffer, ptsUs, &outPreviewImage);
  if (!outPreviewImage) return NULL;
  // lumacore_render_frame hands back a +1 retained CVPixelBufferRef. ARC
  // does not manage CF types (CVPixelBufferRef is not an Objective-C object
  // pointer, so __bridge_transfer doesn't apply here — verified this doesn't
  // even compile). CF_RETURNS_RETAINED on this method documents the +1 for
  // callers/the static analyzer; a plain cast just hands the pointer through.
  return (CVPixelBufferRef)outPreviewImage;
}

- (void)setThermalState:(int64_t)session state:(int32_t)state {
  lumacore_set_thermal_state(session, state);
}

- (BOOL)stopRecording:(int64_t)session {
  return lumacore_stop_recording(session) == 0;
}

- (int32_t)validateLicense:(NSString*)tokenBlobJson deviceFingerprint:(NSString*)deviceFingerprint {
  return lumacore_validate_license(tokenBlobJson.UTF8String, deviceFingerprint.UTF8String);
}

- (void)submitAudioSample:(CMSampleBufferRef)sampleBuffer session:(int64_t)session ptsUs:(int64_t)ptsUs {
  CMBlockBufferRef blockBuffer = NULL;
  AudioBufferList audioBufferList;
  OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
      sampleBuffer, NULL, &audioBufferList, sizeof(audioBufferList), NULL, NULL,
      kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &blockBuffer);
  if (status != noErr || audioBufferList.mNumberBuffers == 0 || !blockBuffer) {
    if (blockBuffer) CFRelease(blockBuffer);
    return;
  }

  const AudioStreamBasicDescription* asbd =
      CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sampleBuffer));
  int sampleRate = asbd ? (int)asbd->mSampleRate : 0;
  int numChannels = asbd ? (int)asbd->mChannelsPerFrame : 0;

  // AVLinearPCMIsNonInterleaved=false in CameraCaptureController's
  // audioSettings guarantees exactly one AudioBuffer here.
  AudioBuffer buffer = audioBufferList.mBuffers[0];
  int numFrames = (asbd && asbd->mBytesPerFrame > 0) ? (int)(buffer.mDataByteSize / asbd->mBytesPerFrame) : 0;

  lumacore_submit_audio_frame(session, buffer.mData, numFrames, sampleRate, numChannels, ptsUs);
  CFRelease(blockBuffer);
}

@end

#endif  // __APPLE__
