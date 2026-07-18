import AVFoundation
import CryptoKit
import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate {
  private let cameraController = CameraCaptureController()
  private let previewTexture = CameraPreviewTexture()
  private let recordingController = RecordingController()
  private let effectsController = EffectsRenderController()
  // Separate from effectsController's internal bridge — license validation
  // is stateless and not tied to a render session's lifecycle.
  private let licenseBridge = LumaCoreBridge()

  // Set once in handleStartCamera from the actually-delivered frame size —
  // recording must encode at the same dimensions the camera is producing.
  private var frameSize: CGSize?

  // Set via the "setRecordingSettings" channel call (Settings screen),
  // consumed on the next startCamera()/startRecording(). Not gated on
  // isConfigured on the CameraCaptureController side, so a resolution change
  // takes effect the next time the Camera tab opens.
  private var pendingBitrateKbps: Int32 = 6000
  private var pendingResolutionPreset: AVCaptureSession.Preset = .hd1920x1080

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
      cameraController.onFrame = { [weak effectsController, weak previewTexture] pixelBuffer, pts in
        guard let rendered = effectsController?.renderFrame(pixelBuffer, pts: pts) else { return }
        previewTexture?.updateFrame(rendered)
        // recordingController is NOT called here — recording (if active)
        // already happened inside lumacore_render_frame, see
        // EffectsRenderController.renderFrame / ai_plans/03 §8.
      }
      cameraController.onAudioSample = { [weak effectsController] sampleBuffer in
        effectsController?.submitAudioSample(sampleBuffer)
      }

      let channel = FlutterMethodChannel(name: "com.lumacore/native", binaryMessenger: controller.binaryMessenger)
      channel.setMethodCallHandler { [weak self] call, result in
        guard let self else { return }
        switch call.method {
        case "getDeviceFingerprint":
          result(self.getDeviceFingerprint())
        case "validateLicense":
          let args = call.arguments as? [String: Any]
          let tokenBlobJson = args?["tokenBlobJson"] as? String ?? ""
          let deviceFingerprint = args?["deviceFingerprint"] as? String ?? ""
          let status = self.licenseBridge.validateLicense(tokenBlobJson, deviceFingerprint: deviceFingerprint)
          result(Int(status))
        case "setRecordingSettings":
          let args = call.arguments as? [String: Any]
          if let bitrateKbps = args?["bitrateKbps"] as? Int {
            self.pendingBitrateKbps = Int32(bitrateKbps)
          }
          if let presetName = args?["resolutionPreset"] as? String {
            self.pendingResolutionPreset = Self.resolutionPreset(named: presetName)
          }
          result(nil)
        case "startCamera":
          self.handleStartCamera(result: result)
        case "stopCamera":
          self.cameraController.stop()
          self.effectsController.stop()
          result(nil)
        case "startRecording":
          self.handleStartRecording(result: result)
        case "stopRecording":
          self.handleStopRecording(result: result)
        case "forceThermalStateForTesting":
          let args = call.arguments as? [String: Any]
          let state = args?["state"] as? Int ?? 0
          self.effectsController.forceThermalStateForTesting(Int32(state))
          result(nil)
        default:
          result(FlutterMethodNotImplemented)
        }
      }
    }

    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  // SHA256(keychainUUID + bundle_id), see ARCHITECTURE.md §6. The server
  // hashes this value a second time when issuing a token
  // (server/app/main.py) — this function must return the once-hashed value,
  // not the raw UUID and not a twice-hashed value.
  private func getDeviceFingerprint() -> String {
    let uuid = KeychainFingerprint.loadOrCreateUUID()
    let bundleId = Bundle.main.bundleIdentifier ?? "com.lumacore.lumacore"
    let digest = SHA256.hash(data: Data((uuid + bundleId).utf8))
    return digest.map { String(format: "%02x", $0) }.joined()
  }

  private static func resolutionPreset(named name: String) -> AVCaptureSession.Preset {
    switch name {
    case "hd720": return .hd1280x720
    case "uhd4k": return .hd4K3840x2160
    default: return .hd1920x1080
    }
  }

  private func handleStartCamera(result: @escaping FlutterResult) {
    if previewTexture.textureId == -1 {
      let textures = self.registrar(forPlugin: "LumaCoreCamera")!.textures()
      previewTexture.registry = textures
      previewTexture.textureId = textures.register(previewTexture)
    }

    cameraController.start(resolutionPreset: pendingResolutionPreset) { [previewTexture, effectsController] captureResult in
      DispatchQueue.main.async {
        switch captureResult {
        case .success(let frameSize):
          self.frameSize = frameSize
          let session = effectsController.start(width: Int(frameSize.width), height: Int(frameSize.height))
          result([
            "textureId": previewTexture.textureId,
            "width": Int(frameSize.width),
            "height": Int(frameSize.height),
            "sessionId": session,
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

    recordingController.start(
      session: effectsController.session,
      width: Int(frameSize.width),
      height: Int(frameSize.height),
      bitrateKbps: pendingBitrateKbps
    ) { startResult in
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
