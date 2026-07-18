import AVFoundation
import Foundation
import Photos

/// Owns the recording lifecycle: start/stop against a render session it
/// borrows from `EffectsRenderController`, and the Documents → Photos
/// handoff on a successful stop. `encodeQueue` is deliberately separate from
/// `CameraCaptureController`'s `captureQueue` — encoding must never block
/// frame delivery to the live preview. See
/// docs/ai_plans/01-ios-ffmpeg-minimal-recording.md §7-8,
/// ai_plans/03-ios-metal-render-pipeline.md §8.
final class RecordingController {
  enum RecordingError: Error {
    case alreadyRecording
    case notRecording
    case startFailed
    case stopFailed
    case photosPermissionDenied
  }

  // Fallback default when the caller doesn't pass an explicit bitrate.
  private static let defaultBitrateKbps: Int32 = 6000

  private let bridge = LumaCoreBridge()
  private let encodeQueue = DispatchQueue(label: "com.lumacore.recording.encode")

  private var session: Int64 = -1
  private var outputURL: URL?
  private var recording = false

  /// `session` comes from `EffectsRenderController` — this controller never
  /// creates or releases it, only borrows it for the duration of a
  /// recording (see ai_plans/03-ios-metal-render-pipeline.md §8: one render
  /// session per camera lifecycle, not per recording).
  func start(session: Int64, width: Int, height: Int, bitrateKbps: Int32 = RecordingController.defaultBitrateKbps,
             completion: @escaping (Result<URL, Error>) -> Void) {
    encodeQueue.async { [weak self] in
      guard let self else { return }
      guard !self.recording else {
        completion(.failure(RecordingError.alreadyRecording))
        return
      }

      let url = Self.makeOutputURL()
      let started = self.bridge.startRecording(
        session,
        outputPath: url.path,
        bitrateKbps: bitrateKbps,
        width: Int32(width),
        height: Int32(height)
      )
      guard started else {
        completion(.failure(RecordingError.startFailed))
        return
      }

      self.session = session
      self.outputURL = url
      self.recording = true
      completion(.success(url))
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
      self.session = -1
      self.outputURL = nil

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
