#include "TokenValidator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <optional>
#include <vector>

#include "ed25519.h"
#include "sha256.h"

namespace lumacore::license {

namespace {

// Only safe for fields guaranteed not to contain an embedded '"' or escape —
// payload_b64/signature_b64 are base64-alphabet strings, which structurally
// guarantees this. Do NOT reuse for arbitrary JSON string fields (e.g. a
// user-controlled `plan` name) without a real JSON parser.
std::optional<std::string> extractJsonStringField(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t keyPos = json.find(needle);
  if (keyPos == std::string::npos) return std::nullopt;
  size_t colon = json.find(':', keyPos + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  size_t firstQuote = json.find('"', colon + 1);
  if (firstQuote == std::string::npos) return std::nullopt;
  size_t secondQuote = json.find('"', firstQuote + 1);
  if (secondQuote == std::string::npos) return std::nullopt;
  return json.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

std::vector<uint8_t> base64Decode(const std::string& in) {
  static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::array<int, 256> lut{};
  lut.fill(-1);
  for (size_t i = 0; i < kAlphabet.size(); ++i) lut[static_cast<uint8_t>(kAlphabet[i])] = static_cast<int>(i);

  std::vector<uint8_t> out;
  int val = 0;
  int bits = -8;
  for (unsigned char c : in) {
    if (c == '=' || lut[c] == -1) continue;
    val = (val << 6) + lut[c];
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

uint64_t readU64BE(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

}  // namespace

TokenValidator::TokenValidator(std::string publicKeyBase64) : publicKeyBase64_(std::move(publicKeyBase64)) {}

LicenseStatus TokenValidator::validate(const std::string& tokenBlobJson, const std::string& deviceFingerprint) const {
  auto payloadB64 = extractJsonStringField(tokenBlobJson, "payload_b64");
  auto signatureB64 = extractJsonStringField(tokenBlobJson, "signature_b64");
  if (!payloadB64 || !signatureB64) return LicenseStatus::NotActivated;

  std::vector<uint8_t> payload = base64Decode(*payloadB64);
  std::vector<uint8_t> signature = base64Decode(*signatureB64);
  std::vector<uint8_t> publicKey = base64Decode(publicKeyBase64_);
  // Canonical layout (server/app/crypto.py::encode_payload — must byte-for-byte
  // match): license_id(16) + fingerprint_hash(32) + issued_at(8 BE) +
  // expires_at(8 BE) + plan_len(1) + plan. 65 = 16+32+8+8+1.
  if (signature.size() != 64 || publicKey.size() != 32 || payload.size() < 65) {
    return LicenseStatus::InvalidSignature;
  }

  if (ed25519_verify(signature.data(), payload.data(), payload.size(), publicKey.data()) != 1) {
    return LicenseStatus::InvalidSignature;
  }

  // Signature verified — safe to trust these bytes now. Re-derive
  // fingerprint_hash/expires_at from the verified payload itself rather than
  // from the token JSON's plaintext copies (those aren't cryptographically
  // bound to anything on their own).
  const uint8_t* p = payload.data();
  uint64_t expiresAt = readU64BE(p + 56);
  uint8_t planLen = p[64];
  if (payload.size() < 65u + planLen) return LicenseStatus::InvalidSignature;

  uint8_t computedHash[32];
  sha256(reinterpret_cast<const uint8_t*>(deviceFingerprint.data()), deviceFingerprint.size(), computedHash);
  if (!std::equal(p + 16, p + 48, computedHash)) return LicenseStatus::DeviceMismatch;

  if (static_cast<uint64_t>(std::time(nullptr)) > expiresAt) return LicenseStatus::Expired;
  return LicenseStatus::Valid;
}

}  // namespace lumacore::license
