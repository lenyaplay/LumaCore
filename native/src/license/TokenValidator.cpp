#include "TokenValidator.h"

namespace lumacore::license {

TokenValidator::TokenValidator(std::string publicKeyBase64) : publicKeyBase64_(std::move(publicKeyBase64)) {}

LicenseStatus TokenValidator::validate(const std::string&, const std::string&) const {
  // TODO(Этап 6): parse token envelope, libsodium crypto_sign_verify_detached
  // on payload_b64/signature_b64, check sha256(fingerprint) + expires_at.
  return LicenseStatus::NotActivated;
}

}  // namespace lumacore::license
