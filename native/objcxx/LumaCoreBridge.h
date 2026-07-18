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
- (void)submitFrame:(int64_t)session pixelBuffer:(CVPixelBufferRef)pixelBuffer ptsUs:(int64_t)ptsUs;
- (BOOL)stopRecording:(int64_t)session;

@end

#endif  // __APPLE__
