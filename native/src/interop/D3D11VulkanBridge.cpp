#ifdef _WIN32

#include "D3D11VulkanBridge.h"

namespace lumacore::interop {

bool D3D11VulkanBridge::createSharedTexture(int, int) {
  // TODO(Этап 10): ID3D11Texture2D with D3D11_RESOURCE_MISC_SHARED_NTHANDLE +
  // IDXGIResource1::CreateSharedHandle.
  return false;
}

void* D3D11VulkanBridge::importIntoVulkan() {
  // TODO: VkExternalMemoryImageCreateInfo + vkAllocateMemory with
  // VkImportMemoryWin32HandleInfoKHR on the same handle.
  return nullptr;
}

void D3D11VulkanBridge::destroy() {}

}  // namespace lumacore::interop

#endif  // _WIN32
