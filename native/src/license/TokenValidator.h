#pragma once

#include <cstdint>
#include <string>

namespace lumacore::license {

enum class LicenseStatus { Valid, Expired, DeviceMismatch, InvalidSignature, NotActivated };

// Offline Ed25519 verification of the activation token issued by server/
// (ARCHITECTURE.md §6). Only the public key is embedded in the binary; no
// network access after activation.
class TokenValidator {
 public:
  explicit TokenValidator(std::string publicKeyBase64);

  LicenseStatus validate(const std::string& tokenBlobJson, const std::string& deviceFingerprint) const;

 private:
  std::string publicKeyBase64_;
};

}  // namespace lumacore::license
