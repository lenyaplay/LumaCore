import Flutter
import Foundation

/// iOS pull-model texture (ARCHITECTURE.md §3 "iOS — pull"): Flutter's raster
/// thread calls `copyPixelBuffer()` synchronously on demand. Holds only the
/// latest ready CVPixelBuffer — atomic swap, no queueing.
final class CameraPreviewTexture: NSObject, FlutterTexture {
  var textureId: Int64 = -1
  weak var registry: FlutterTextureRegistry?

  private let lock = NSLock()
  private var latestPixelBuffer: CVPixelBuffer?

  func updateFrame(_ pixelBuffer: CVPixelBuffer) {
    lock.lock()
    latestPixelBuffer = pixelBuffer
    lock.unlock()
    registry?.textureFrameAvailable(textureId)
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    let buffer = latestPixelBuffer
    lock.unlock()
    guard let buffer else { return nil }
    return Unmanaged.passRetained(buffer)
  }
}
