package com.lumacore.lumacore

import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

// Platform Channel side of the boundary in ARCHITECTURE.md §3: license
// activation/status, camera lifecycle (open/close/switch), gallery/export,
// device fingerprint (ANDROID_ID). The render loop itself (camera -> C++ ->
// GPU -> texture -> encoder) does not go through here — dart:ffi + JNI only.
class MainActivity : FlutterActivity() {
    private val channelName = "com.lumacore/native"

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName)
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "getDeviceFingerprint" -> {
                        // TODO(Этап 6): SHA256(ANDROID_ID + package_name), see ARCHITECTURE.md §6.
                        result.notImplemented()
                    }
                    else -> result.notImplemented()
                }
            }
    }
}
