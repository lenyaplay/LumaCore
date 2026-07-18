import AVFoundation
import Foundation
import Photos

/// Owns the recording lifecycle: the LumaCore encoder session, its own
/// serial queue, relative-PTS bookkeeping, and the Documents → Photos handoff
/// on a successful stop. `encodeQueue` is deliberately separate from
/// `CameraCaptureController`'s `captureQueue` — encoding must never block
/// frame delivery to the live preview. See
/// docs/ai_plans/01-ios-ffmpeg-minimal-recording.md §7-8.
final class RecordingController {
  enum RecordingError: Error {
    case alreadyRecording
    case notRecording
    case startFailed
    case stopFailed
    case photosPermissionDenied
  }

  // Minimal-milestone constant — no bitrate UI yet.
  private static let bitrateKbps: Int32 = 6000

  private let bridge = LumaCoreBridge()
  private let encodeQueue = DispatchQueue(label: "com.lumacore.recording.encode")

  private var session: Int64 = -1
  private var outputURL: URL?
  private var firstPts: CMTime?
  private var recording = false

  func start(width: Int, height: Int, completion: @escaping (Result<URL, Error>) -> Void) {
    encodeQueue.async { [weak self] in
      guard let self else { return }
      guard !self.recording else {
        completion(.failure(RecordingError.alreadyRecording))
        return
      }

      let url = Self.makeOutputURL()
      // No Metal/RenderPipeline yet (Этап 4) — this session exists solely to
      // own the EncoderSession; renderInit's platformSurfaceOrCtx is unused
      // on this passthrough path.
      let newSession = self.bridge.renderInit(withContext: nil, width: Int32(width), height: Int32(height))
      let started = self.bridge.startRecording(
        newSession,
        outputPath: url.path,
        bitrateKbps: Self.bitrateKbps,
        width: Int32(width),
        height: Int32(height)
      )
      guard started else {
        self.bridge.release(newSession)
        completion(.failure(RecordingError.startFailed))
        return
      }

      self.session = newSession
      self.outputURL = url
      self.firstPts = nil
      self.recording = true
      completion(.success(url))
    }
  }

  /// Call from `CameraCaptureController.onFrame`. Safe to call whether or not
  /// a recording is active — a no-op when it isn't.
  func submitFrame(_ pixelBuffer: CVPixelBuffer, pts: CMTime) {
    encodeQueue.async { [weak self] in
      guard let self, self.recording else { return }
      if self.firstPts == nil {
        self.firstPts = pts
      }
      let relativePts = CMTimeSubtract(pts, self.firstPts!)
      let ptsUs = Int64((CMTimeGetSeconds(relativePts) * 1_000_000).rounded())
      self.bridge.submitFrame(self.session, pixelBuffer: pixelBuffer, ptsUs: ptsUs)
    }
  }

  func stop(completion: @escaping (Result<URL, Error>) -> Void) {
    encodeQueue.async { [weak self] in
      guard let self else { return }
      guard self.recording, let url = self.outputURL else {
        completion(.failure(RecordingError.notRecording))
        return
      }
      self.recording = false
      let stopped = self.bridge.stopRecording(self.session)
      self.bridge.release(self.session)
      self.session = -1
      self.outputURL = nil
      self.firstPts = nil

      guard stopped else {
        completion(.failure(RecordingError.stopFailed))
        return
      }

      Self.saveToPhotos(url) { result in
        switch result {
        case .success:
          completion(.success(url))
        case .failure(let error):
          completion(.failure(error))
        }
      }
    }
  }

  private static func makeOutputURL() -> URL {
    let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    let timestamp = Int(Date().timeIntervalSince1970 * 1000)
    return documents.appendingPathComponent("lumacore_\(timestamp).mp4")
  }

  // Add-only authorization (NSPhotoLibraryAddUsageDescription) — never
  // requests full library read/write access.
  private static func saveToPhotos(_ url: URL, completion: @escaping (Result<Void, Error>) -> Void) {
    PHPhotoLibrary.requestAuthorization(for: .addOnly) { status in
      guard status == .authorized || status == .limited else {
        completion(.failure(RecordingError.photosPermissionDenied))
        return
      }
      PHPhotoLibrary.shared().performChanges({
        PHAssetCreationRequest.forAsset().addResource(with: .video, fileURL: url, options: nil)
      }, completionHandler: { success, error in
        if success {
          completion(.success(()))
        } else {
          completion(.failure(error ?? RecordingError.stopFailed))
        }
      })
    }
  }
}
