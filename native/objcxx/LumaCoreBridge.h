#pragma once

#ifdef __APPLE__

#import <Foundation/Foundation.h>

// Thin Obj-C++ layer only — no logic here, everything forwards to
// lumacore_api.h. See ARCHITECTURE.md §5. Stub — implemented alongside
// AVFoundation glue in Этап 2.
@interface LumaCoreBridge : NSObject

- (int64_t)renderInitWithContext:(void*)ctx width:(int)width height:(int)height;
- (void)release:(int64_t)session;

@end

#endif  // __APPLE__
