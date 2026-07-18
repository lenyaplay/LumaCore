import AVFoundation
import Foundation

/// Owns the AVCaptureSession. Raw passthrough only — no GPU processing here,
/// that lands with the Metal RenderPipeline in Этап 4 (ARCHITECTURE.md §1/§2).
final class CameraCaptureController: NSObject {
  enum CaptureError: Error {
    case permissionDenied
    case noCaptureDevice
    case configurationFailed
  }

  /// Delivered on `captureQueue` — never the main thread. The CMTime is the
  /// camera's own hardware presentation timestamp (not wall-clock) — needed
  /// as the recording PTS source, see RecordingController.
  var onFrame: ((CVPixelBuffer, CMTime) -> Void)?

  /// Delivered on `audioQueue` — a separate serial queue from `captureQueue`,
  /// so a slow audio consumer never blocks frame delivery to the live
  /// preview (and vice versa).
  var onAudioSample: ((CMSampleBuffer) -> Void)?

  private let session = AVCaptureSession()
  private let captureQueue = DispatchQueue(label: "com.lumacore.camera.capture")
  private let audioQueue = DispatchQueue(label: "com.lumacore.camera.audio")
  private var isConfigured = false

  // Resolved from the first delivered frame, not assumed from activeFormat —
  // reflects whatever rotation the connection actually applied.
  private var pendingStartCompletion: ((Result<CGSize, Error>) -> Void)?

  func start(resolutionPreset: AVCaptureSession.Preset = .hd1920x1080,
             completion: @escaping (Result<CGSize, Error>) -> Void) {
    requestPermissionIfNeeded { [weak self] granted in
      guard let self else { return }
      guard granted else {
        completion(.failure(CaptureError.permissionDenied))
        return
      }
      self.captureQueue.async {
        do {
          if !self.isConfigured {
            try self.configureSession()
            self.isConfigured = true
          }
          // Not gated on isConfigured — applied on every start() (cheap
          // no-op if unchanged), so a resolution change picked in Settings
          // takes effect the next time the Camera tab is opened, without
          // needing a full session teardown/rebuild.
          self.applyResolutionPreset(resolutionPreset)
          self.pendingStartCompletion = completion
          self.session.startRunning()
        } catch {
          completion(.failure(error))
        }
      }
    }
  }

  private func applyResolutionPreset(_ preset: AVCaptureSession.Preset) {
    guard session.canSetSessionPreset(preset) else { return }
    session.beginConfiguration()
    session.sessionPreset = preset
    session.commitConfiguration()
  }

  func stop() {
    captureQueue.async { [session] in
      if session.isRunning {
        session.stopRunning()
      }
    }
  }

  // Video, then audio — the camera does not start at all without both
  // grants (no video-only fallback; kept simple for this app's scope).
  private func requestPermissionIfNeeded(_ completion: @escaping (Bool) -> Void) {
    requestPermission(for: .video) { [weak self] videoGranted in
      guard let self, videoGranted else {
        completion(false)
        return
      }
      self.requestPermission(for: .audio) { audioGranted in
        completion(audioGranted)
      }
    }
  }

  private func requestPermission(for mediaType: AVMediaType, completion: @escaping (Bool) -> Void) {
    switch AVCaptureDevice.authorizationStatus(for: mediaType) {
    case .authorized:
      completion(true)
    case .notDetermined:
      AVCaptureDevice.requestAccess(for: mediaType) { granted in
        completion(granted)
      }
    case .denied, .restricted:
      completion(false)
    @unknown default:
      completion(false)
    }
  }

  private func configureSession() throws {
    guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else {
      throw CaptureError.noCaptureDevice
    }

    session.beginConfiguration()
    defer { session.commitConfiguration() }

    let input = try AVCaptureDeviceInput(device: device)
    guard session.canAddInput(input) else {
      throw CaptureError.configurationFailed
    }
    session.addInput(input)

    let output = AVCaptureVideoDataOutput()
    output.alwaysDiscardsLateVideoFrames = true
    // Pinned explicitly (not left to the AVFoundation default) so it maps
    // 1:1 onto FFmpeg's AV_PIX_FMT_NV12 (full-range) in EncoderSession — see
    // ai_plans/01-ios-ffmpeg-minimal-recording.md §4.
    output.videoSettings = [
      kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
    ]
    output.setSampleBufferDelegate(self, queue: captureQueue)
    guard session.canAddOutput(output) else {
      throw CaptureError.configurationFailed
    }
    session.addOutput(output)

    // The sensor buffer arrives in its native (landscape) orientation
    // regardless of device rotation — the UI is portrait-locked (Info.plist),
    // so the connection must be pinned to portrait explicitly, not derived
    // from live device orientation.
    if let connection = output.connection(with: .video) {
      if #available(iOS 17.0, *), connection.isVideoRotationAngleSupported(90) {
        connection.videoRotationAngle = 90
      } else if connection.isVideoOrientationSupported {
        connection.videoOrientation = .portrait
      }
    }

    guard let audioDevice = AVCaptureDevice.default(for: .audio) else {
      throw CaptureError.noCaptureDevice
    }
    let audioInput = try AVCaptureDeviceInput(device: audioDevice)
    guard session.canAddInput(audioInput) else {
      throw CaptureError.configurationFailed
    }
    session.addInput(audioInput)

    let audioOutput = AVCaptureAudioDataOutput()
    // AVCaptureAudioDataOutput.audioSettings is API_UNAVAILABLE on iOS
    // (macOS-only) — on iOS the output always vends samples in the
    // device's native format. LumaCoreBridge.submitAudioSample: reads the
    // actual sample rate/channel count from each CMSampleBuffer's
    // AudioStreamBasicDescription at runtime instead of assuming a fixed
    // format; EncoderSession::submitAudioFrame resamples whatever arrives
    // into the AAC encoder's fixed 44.1kHz mono via libswresample.
    audioOutput.setSampleBufferDelegate(self, queue: audioQueue)
    guard session.canAddOutput(audioOutput) else {
      throw CaptureError.configurationFailed
    }
    session.addOutput(audioOutput)
  }
}

extension CameraCaptureController: AVCaptureVideoDataOutputSampleBufferDelegate,
  AVCaptureAudioDataOutputSampleBufferDelegate
{
  func captureOutput(
    _ output: AVCaptureOutput,
    didOutput sampleBuffer: CMSampleBuffer,
    from connection: AVCaptureConnection
  ) {
    if output is AVCaptureAudioDataOutput {
      onAudioSample?(sampleBuffer)
      return
    }

    guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

    if let completion = pendingStartCompletion {
      pendingStartCompletion = nil
      let size = CGSize(width: CVPixelBufferGetWidth(pixelBuffer), height: CVPixelBufferGetHeight(pixelBuffer))
      completion(.success(size))
    }

    let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
    onFrame?(pixelBuffer, pts)
  }
}
