#pragma once

#ifdef _WIN32

#include <string>

namespace lumacore::win32 {

// Self-generated UUID persisted via DPAPI (CryptProtectData, user-scope) at
// %LOCALAPPDATA%\LumaCore\device_id.bin, mixed with MachineGuid as entropy
// only (not as the identity itself — avoids cloned-VM collisions). See
// ARCHITECTURE.md §6. Stub — implemented in Этап 10.
std::string getOrCreateDeviceFingerprint();

}  // namespace lumacore::win32

#endif  // _WIN32
