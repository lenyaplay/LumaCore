/// Placeholder for ffigen-generated bindings to native/src/api/lumacore_api.h.
/// dart:ffi wiring (DynamicLibrary.open + generated bindings, registration of
/// the preview texture, effect params, start/stop recording) lands once the
/// native library actually builds for each platform — Этап 2/3.
/// ARCHITECTURE.md §3.
class LumaCoreBindings {
  // TODO(Этап 2/3): ffigen --config ffigen.yaml against native/src/api/lumacore_api.h,
  // then DynamicLibrary.open('liblumacore.so' / 'lumacore.dll' / process lookup on iOS).
}
