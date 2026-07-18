import 'package:flutter/services.dart';

/// Texture id plus the actual delivered frame size — the native side reports
/// the buffer dimensions it observed after rotation, not an assumed size, so
/// the Flutter side can render with the correct aspect ratio.
class CameraStartResult {
  const CameraStartResult({required this.textureId, required this.width, required this.height});

  final int textureId;
  final int width;
  final int height;
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
}
