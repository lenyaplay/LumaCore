#ifdef _WIN32

#include "DpapiFingerprint.h"

namespace lumacore::win32 {

std::string getOrCreateDeviceFingerprint() {
  // TODO(Этап 10): read/create %LOCALAPPDATA%\LumaCore\device_id.bin via
  // CryptProtectData, read HKLM MachineGuid as extra entropy, SHA256 combine.
  return {};
}

}  // namespace lumacore::win32

#endif  // _WIN32
