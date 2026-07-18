#pragma once

#ifdef _WIN32

namespace lumacore::interop {

// Shared NT-handle bridge between the app's own ID3D11Device and the Vulkan
// render backend, so the final render pass can be presented through Flutter's
// FlutterDesktopGpuSurfaceDescriptor (D3D11 texture) even though rendering
// itself is Vulkan. See ARCHITECTURE.md §3. Stub — implemented in Этап 10.
class D3D11VulkanBridge {
 public:
  bool createSharedTexture(int width, int height);
  void* importIntoVulkan();
  void destroy();
};

}  // namespace lumacore::interop

#endif  // _WIN32
