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

- (void)submitFrame:(int64_t)session pixelBuffer:(CVPixelBufferRef)pixelBuffer ptsUs:(int64_t)ptsUs {
  lumacore_submit_frame(session, (void*)pixelBuffer, ptsUs);
}

- (BOOL)stopRecording:(int64_t)session {
  return lumacore_stop_recording(session) == 0;
}

@end

#endif  // __APPLE__
