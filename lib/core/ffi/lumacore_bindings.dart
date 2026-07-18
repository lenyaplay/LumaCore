import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'lumacore_bindings_generated.dart';

/// Dart-side mirror of the C `LumaEffectParams` struct.
class LumaEffectParamsDart {
  const LumaEffectParamsDart({
    required this.brightness,
    required this.contrast,
    required this.saturation,
    required this.vignetteRadius,
    required this.vignetteSoftness,
    required this.particleIntensity,
    required this.effectMask,
  });

  final double brightness;
  final double contrast;
  final double saturation;
  final double vignetteRadius;
  final double vignetteSoftness;
  final double particleIntensity;
  final int effectMask;
}

/// Dart-side mirror of the C `LumaStats` struct.
class LumaStatsDart {
  const LumaStatsDart({
    required this.fps,
    required this.avgFrameMs,
    required this.droppedFrames,
    required this.thermalState,
  });

  final double fps;
  final double avgFrameMs;
  final int droppedFrames;
  final int thermalState;
}

/// Thin wrapper over the ffigen-generated bindings. Only the two
/// high-frequency calls the effects UI needs are exposed here — session
/// lifecycle, per-frame rendering, and recording control stay Obj-C++/Swift
/// only and never go through dart:ffi (ARCHITECTURE.md §3).
class LumaCoreBindings {
  LumaCoreBindings._() : _bindings = LumaCoreBindingsGenerated(DynamicLibrary.process());

  static final instance = LumaCoreBindings._();

  final LumaCoreBindingsGenerated _bindings;

  void setEffectParams(int sessionId, LumaEffectParamsDart params) {
    final ptr = calloc<LumaEffectParams>();
    try {
      ptr.ref
        ..brightness = params.brightness
        ..contrast = params.contrast
        ..saturation = params.saturation
        ..vignetteRadius = params.vignetteRadius
        ..vignetteSoftness = params.vignetteSoftness
        ..particleIntensity = params.particleIntensity
        ..effectMask = params.effectMask;
      _bindings.lumacore_set_effect_params(sessionId, ptr);
    } finally {
      calloc.free(ptr);
    }
  }

  LumaStatsDart getStats(int sessionId) {
    final ptr = calloc<LumaStats>();
    try {
      _bindings.lumacore_get_stats(sessionId, ptr);
      return LumaStatsDart(
        fps: ptr.ref.fps,
        avgFrameMs: ptr.ref.avgFrameMs,
        droppedFrames: ptr.ref.droppedFrames,
        thermalState: ptr.ref.thermalState,
      );
    } finally {
      calloc.free(ptr);
    }
  }
}

// DynamicLibrary.process() is statically linked into the binary on iOS, not
// a separate .dylib. The project is iOS-only right now — a
// Platform.isAndroid ? DynamicLibrary.open(...) : DynamicLibrary.process()
// branch is deliberately not added until Android support lands.
