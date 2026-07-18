import AVFoundation
import Foundation

/// Owns the render session for the camera's entire lifecycle (created in
/// startCamera, destroyed in stopCamera) — NOT recreated per recording.
/// Metal setup (device/pipeline states/metallib load/pools) is an expensive
/// one-time cost, and the live preview must show effects even when no
/// recording is active — owning the session here rather than in
/// RecordingController is what makes that possible. See
/// ai_plans/03-ios-metal-render-pipeline.md §8.
final class EffectsRenderController {
  private let bridge = LumaCoreBridge()
  private(set) var session: Int64 = -1

  func start(width: Int, height: Int) -> Int64 {
    session = bridge.renderInit(withContext: nil, width: Int32(width), height: Int32(height))
    observeThermalState()
    return session
  }

  /// Call from CameraCaptureController.onFrame. Runs the frame through the
  /// full effects pipeline and — if a recording is active — forwards it to
  /// the encoder internally (see lumacore_render_frame). Returns the
  /// rendered frame for preview, or nil if the frame was dropped.
  func renderFrame(_ pixelBuffer: CVPixelBuffer, pts: CMTime) -> CVPixelBuffer? {
    guard session != -1 else { return nil }
    let ptsUs = Int64((CMTimeGetSeconds(pts) * 1_000_000).rounded())
    return bridge.renderFrame(session, pixelBuffer: pixelBuffer, ptsUs: ptsUs)
  }

  /// Debug-only hook (see camera_screen.dart's long-press-to-throttle
  /// overlay) — forces the reported thermal state without real device heat,
  /// so the particles-off-at-state>=2 ladder can be demoed on demand.
  func forceThermalStateForTesting(_ state: Int32) {
    guard session != -1 else { return }
    bridge.setThermalState(session, state: state)
  }

  private func observeThermalState() {
    NotificationCenter.default.addObserver(
      forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: nil
    ) { [weak self] _ in
      guard let self, self.session != -1 else { return }
      self.bridge.setThermalState(self.session, state: Int32(ProcessInfo.processInfo.thermalState.rawValue))
    }
    bridge.setThermalState(session, state: Int32(ProcessInfo.processInfo.thermalState.rawValue))
  }

  func stop() {
    NotificationCenter.default.removeObserver(self, name: ProcessInfo.thermalStateDidChangeNotification, object: nil)
    if session != -1 {
      bridge.release(session)
      session = -1
    }
  }
}
