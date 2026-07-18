#ifdef __APPLE__

#import "LumaCoreBridge.h"

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

@end

#endif  // __APPLE__
