import 'package:flutter/services.dart';

/// Texture id plus the actual delivered frame size — the native side reports
/// the buffer dimensions it observed after rotation, not an assumed size, so
/// the Flutter side can render with the correct aspect ratio.
class CameraStartResult {
  const CameraStartResult({
    required this.textureId,
    required this.width,
    required this.height,
    required this.sessionId,
  });

  final int textureId;
  final int width;
  final int height;
  // The render session id lumacore_render_init already assigned natively
  // (EffectsRenderController.start, ai_plans/03-ios-metal-render-pipeline.md
  // §8/§9) — Dart never creates or releases this session, only uses the id
  // for dart:ffi calls (setEffectParams/getStats) until stopCamera().
  final int sessionId;
}

/// Platform Channel boundary for calls that must originate from Kotlin/Swift/
/// C++ (Windows): license activation, camera lifecycle (open/close/switch),
/// gallery/export, device fingerprint. The high-frequency render loop does
/// NOT go through here — see core/ffi/. ARCHITECTURE.md §3.
class NativeChannel {
  NativeChannel._();

  static const _channel = MethodChannel('com.lumacore/native');

  static Future<String> getDeviceFingerprint() async {
    final result = await _channel.invokeMethod<String>('getDeviceFingerprint');
    return result ?? '';
  }

  /// Offline license verification (ARCHITECTURE.md §6) — no network access.
  /// Returns a LumaLicenseStatus ordinal (0=Valid, 1=Expired,
  /// 2=DeviceMismatch, 3=InvalidSignature, 4=NotActivated). This is
  /// session-lifecycle-adjacent, not a per-frame call, so it goes through the
  /// Platform Channel rather than dart:ffi.
  static Future<int> validateLicense(String tokenBlobJson, String deviceFingerprint) async {
    final result = await _channel.invokeMethod<int>('validateLicense', {
      'tokenBlobJson': tokenBlobJson,
      'deviceFingerprint': deviceFingerprint,
    });
    return result ?? 4; // NotActivated
  }

  /// Stores the bitrate/resolution to use for the next startCamera()
  /// (resolution)/startRecording() (bitrate) call — call before startCamera()
  /// so the preset is applied when the capture session is configured.
  static Future<void> setRecordingSettings({required int bitrateKbps, required String resolutionPreset}) async {
    await _channel.invokeMethod('setRecordingSettings', {
      'bitrateKbps': bitrateKbps,
      'resolutionPreset': resolutionPreset,
    });
  }

  /// Starts the native camera capture session. Throws [PlatformException] on
  /// permission denial or capture-device failure.
  static Future<CameraStartResult> startCamera() async {
    final result = await _channel.invokeMapMethod<String, dynamic>('startCamera');
    if (result == null) {
      throw PlatformException(code: 'NO_RESULT', message: 'startCamera returned no result');
    }
    return CameraStartResult(
      textureId: result['textureId'] as int,
      width: result['width'] as int,
      height: result['height'] as int,
      sessionId: result['sessionId'] as int,
    );
  }

  static Future<void> stopCamera() async {
    await _channel.invokeMethod('stopCamera');
  }

  /// Starts recording the active camera session to a file, returning its
  /// path. Throws [PlatformException] if the camera isn't started or the
  /// encoder fails to initialize.
  static Future<String> startRecording() async {
    final result = await _channel.invokeMethod<String>('startRecording');
    if (result == null) {
      throw PlatformException(code: 'NO_RESULT', message: 'startRecording returned no result');
    }
    return result;
  }

  /// Stops the active recording. The recorded file is saved to Photos
  /// natively before this resolves.
  static Future<void> stopRecording() async {
    await _channel.invokeMethod('stopRecording');
  }

  /// Debug-only: forces the render session's reported thermal state without
  /// actually overheating the device, so the thermal-throttling ladder
  /// (particles disabled at state >= 2) can be verified on demand. This is
  /// session-lifecycle-adjacent control, not a per-frame call, so it goes
  /// through the Platform Channel rather than dart:ffi — see
  /// ai_plans/03-ios-metal-render-pipeline.md §9/§11 p.7.
  static Future<void> forceThermalStateForTesting(int state) async {
    await _channel.invokeMethod('forceThermalStateForTesting', {'state': state});
  }
}
