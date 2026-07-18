import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate {
  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    GeneratedPluginRegistrant.register(with: self)

    // Platform Channel side of the boundary in ARCHITECTURE.md §3: license
    // activation/status, camera lifecycle, gallery/export, device fingerprint
    // (Keychain UUID). The render loop itself does not go through here —
    // dart:ffi + Obj-C++ bridge only.
    if let controller = window?.rootViewController as? FlutterViewController {
      let channel = FlutterMethodChannel(name: "com.lumacore/native", binaryMessenger: controller.binaryMessenger)
      channel.setMethodCallHandler { call, result in
        switch call.method {
        case "getDeviceFingerprint":
          // TODO(Этап 6): SHA256(keychainUUID + bundle_id), see ARCHITECTURE.md §6.
          result(FlutterMethodNotImplemented)
        default:
          result(FlutterMethodNotImplemented)
        }
      }
    }

    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }
}
