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

@end

#endif  // __APPLE__
