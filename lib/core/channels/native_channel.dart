import 'package:flutter/services.dart';

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
}
