#pragma once

#ifdef __APPLE__

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

// Thin Obj-C++ layer only — no logic here, everything forwards to
// lumacore_api.h. See ARCHITECTURE.md §5.
@interface LumaCoreBridge : NSObject

- (int64_t)renderInitWithContext:(void*)ctx width:(int)width height:(int)height;
- (void)release:(int64_t)session;

- (BOOL)startRecording:(int64_t)session
             outputPath:(NSString*)outputPath
            bitrateKbps:(int)bitrateKbps
                  width:(int)width
                 height:(int)height;
// Runs pixelBuffer through the full effects pipeline and, if a recording is
// active, forwards it to the encoder internally (see lumacore_render_frame).
// Returns the rendered preview frame, or nil if the frame was dropped.
- (nullable CVPixelBufferRef)renderFrame:(int64_t)session
                              pixelBuffer:(CVPixelBufferRef)pixelBuffer
                                    ptsUs:(int64_t)ptsUs CF_RETURNS_RETAINED;
- (void)setThermalState:(int64_t)session state:(int32_t)state;
- (BOOL)stopRecording:(int64_t)session;

@end

#endif  // __APPLE__
