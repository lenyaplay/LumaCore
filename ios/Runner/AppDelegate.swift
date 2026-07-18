import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate {
  private let cameraController = CameraCaptureController()
  private let previewTexture = CameraPreviewTexture()
  private let recordingController = RecordingController()

  // Set once in handleStartCamera from the actually-delivered frame size —
  // recording must encode at the same dimensions the camera is producing.
  private var frameSize: CGSize?

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
      cameraController.onFrame = { [weak previewTexture, weak recordingController] pixelBuffer, pts in
        previewTexture?.updateFrame(pixelBuffer)
        recordingController?.submitFrame(pixelBuffer, pts: pts)
      }

      let channel = FlutterMethodChannel(name: "com.lumacore/native", binaryMessenger: controller.binaryMessenger)
      channel.setMethodCallHandler { [weak self] call, result in
        guard let self else { return }
        switch call.method {
        case "getDeviceFingerprint":
          // TODO(Этап 6): SHA256(keychainUUID + bundle_id), see ARCHITECTURE.md §6.
          result(FlutterMethodNotImplemented)
        case "startCamera":
          self.handleStartCamera(result: result)
        case "stopCamera":
          self.cameraController.stop()
          result(nil)
        case "startRecording":
          self.handleStartRecording(result: result)
        case "stopRecording":
          self.handleStopRecording(result: result)
        default:
          result(FlutterMethodNotImplemented)
        }
      }
    }

    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  private func handleStartCamera(result: @escaping FlutterResult) {
    if previewTexture.textureId == -1 {
      let textures = self.registrar(forPlugin: "LumaCoreCamera")!.textures()
      previewTexture.registry = textures
      previewTexture.textureId = textures.register(previewTexture)
    }

    cameraController.start { [previewTexture] captureResult in
      DispatchQueue.main.async {
        switch captureResult {
        case .success(let frameSize):
          self.frameSize = frameSize
          result([
            "textureId": previewTexture.textureId,
            "width": Int(frameSize.width),
            "height": Int(frameSize.height),
          ])
        case .failure(let error):
          result(FlutterError(
            code: "CAMERA_START_FAILED",
            message: "\(error)",
            details: nil
          ))
        }
      }
    }
  }

  private func handleStartRecording(result: @escaping FlutterResult) {
    guard let frameSize else {
      result(FlutterError(
        code: "CAMERA_NOT_STARTED",
        message: "startRecording called before startCamera",
        details: nil
      ))
      return
    }

    recordingController.start(width: Int(frameSize.width), height: Int(frameSize.height)) { startResult in
      DispatchQueue.main.async {
        switch startResult {
        case .success(let url):
          result(url.path)
        case .failure(let error):
          result(FlutterError(
            code: "RECORDING_START_FAILED",
            message: "\(error)",
            details: nil
          ))
        }
      }
    }
  }

  private func handleStopRecording(result: @escaping FlutterResult) {
    recordingController.stop { stopResult in
      DispatchQueue.main.async {
        switch stopResult {
        case .success:
          result(nil)
        case .failure(let error):
          result(FlutterError(
            code: "RECORDING_STOP_FAILED",
            message: "\(error)",
            details: nil
          ))
        }
      }
    }
  }
}
