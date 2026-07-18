// Pure-logic unit test, no external framework — runs on any host CI without
// devices/toolchains (ARCHITECTURE.md §5). Each check prints PASS/FAIL and the
// process exits non-zero on any failure, which is all CTest needs.
//
// Builds a token payload matching server/app/crypto.py::encode_payload()'s
// canonical layout by hand, signs it with a test-only keypair (vendored
// ed25519_create_keypair — production code only ever calls ed25519_verify),
// and exercises TokenValidator::validate() against deliberately-broken
// variants.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "ed25519.h"
#include "license/TokenValidator.h"
#include "sha256.h"

using lumacore::license::LicenseStatus;
using lumacore::license::TokenValidator;

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
  if (condition) {
    std::printf("PASS: %s\n", description);
  } else {
    std::printf("FAIL: %s\n", description);
    ++g_failures;
  }
}

std::string base64Encode(const uint8_t* data, size_t len) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += kAlphabet[(n >> 18) & 0x3F];
    out += kAlphabet[(n >> 12) & 0x3F];
    out += kAlphabet[(n >> 6) & 0x3F];
    out += kAlphabet[n & 0x3F];
  }
  size_t remaining = len - i;
  if (remaining == 1) {
    uint32_t n = data[i] << 16;
    out += kAlphabet[(n >> 18) & 0x3F];
    out += kAlphabet[(n >> 12) & 0x3F];
    out += "==";
  } else if (remaining == 2) {
    uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
    out += kAlphabet[(n >> 18) & 0x3F];
    out += kAlphabet[(n >> 12) & 0x3F];
    out += kAlphabet[(n >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

void appendU64BE(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// Mirrors server/app/crypto.py::encode_payload byte-for-byte.
std::vector<uint8_t> encodePayload(const uint8_t licenseId[16], const uint8_t fingerprintHash[32],
                                    uint64_t issuedAt, uint64_t expiresAt, const std::string& plan) {
  std::vector<uint8_t> out;
  out.insert(out.end(), licenseId, licenseId + 16);
  out.insert(out.end(), fingerprintHash, fingerprintHash + 32);
  appendU64BE(out, issuedAt);
  appendU64BE(out, expiresAt);
  out.push_back(static_cast<uint8_t>(plan.size()));
  out.insert(out.end(), plan.begin(), plan.end());
  return out;
}

std::string makeTokenJson(const std::string& payloadB64, const std::string& signatureB64) {
  return "{\"payload_b64\":\"" + payloadB64 + "\",\"signature_b64\":\"" + signatureB64 + "\"}";
}

}  // namespace

int main() {
  // Fixed seed — deterministic test keypair, not a real secret.
  uint8_t seed[32];
  for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(i * 7 + 1);
  uint8_t publicKey[32], privateKey[64];
  ed25519_create_keypair(publicKey, privateKey, seed);
  std::string publicKeyB64 = base64Encode(publicKey, 32);

  const std::string fingerprint = "test-device-fingerprint";
  uint8_t fingerprintHash[32];
  sha256(reinterpret_cast<const uint8_t*>(fingerprint.data()), fingerprint.size(), fingerprintHash);

  uint8_t licenseId[16];
  for (int i = 0; i < 16; ++i) licenseId[i] = static_cast<uint8_t>(0xA0 + i);

  uint64_t now = static_cast<uint64_t>(std::time(nullptr));

  TokenValidator validator(publicKeyB64);

  // --- Valid token ---
  {
    auto payload = encodePayload(licenseId, fingerprintHash, now, now + 3600, "portfolio-demo");
    uint8_t signature[64];
    ed25519_sign(signature, payload.data(), payload.size(), publicKey, privateKey);
    std::string json = makeTokenJson(base64Encode(payload.data(), payload.size()), base64Encode(signature, 64));
    check(validator.validate(json, fingerprint) == LicenseStatus::Valid, "valid token -> Valid");
  }

  // --- Expired token ---
  {
    auto payload = encodePayload(licenseId, fingerprintHash, now - 7200, now - 3600, "portfolio-demo");
    uint8_t signature[64];
    ed25519_sign(signature, payload.data(), payload.size(), publicKey, privateKey);
    std::string json = makeTokenJson(base64Encode(payload.data(), payload.size()), base64Encode(signature, 64));
    check(validator.validate(json, fingerprint) == LicenseStatus::Expired, "expired token -> Expired");
  }

  // --- Wrong fingerprint ---
  {
    auto payload = encodePayload(licenseId, fingerprintHash, now, now + 3600, "portfolio-demo");
    uint8_t signature[64];
    ed25519_sign(signature, payload.data(), payload.size(), publicKey, privateKey);
    std::string json = makeTokenJson(base64Encode(payload.data(), payload.size()), base64Encode(signature, 64));
    check(validator.validate(json, "some-other-device") == LicenseStatus::DeviceMismatch,
          "wrong fingerprint -> DeviceMismatch");
  }

  // --- Corrupted signature ---
  {
    auto payload = encodePayload(licenseId, fingerprintHash, now, now + 3600, "portfolio-demo");
    uint8_t signature[64];
    ed25519_sign(signature, payload.data(), payload.size(), publicKey, privateKey);
    signature[0] ^= 0xFF;  // flip bits — must fail verification
    std::string json = makeTokenJson(base64Encode(payload.data(), payload.size()), base64Encode(signature, 64));
    check(validator.validate(json, fingerprint) == LicenseStatus::InvalidSignature,
          "corrupted signature -> InvalidSignature");
  }

  // --- Tampered payload (signature no longer matches) ---
  {
    auto payload = encodePayload(licenseId, fingerprintHash, now, now + 3600, "portfolio-demo");
    uint8_t signature[64];
    ed25519_sign(signature, payload.data(), payload.size(), publicKey, privateKey);
    payload[0] ^= 0xFF;  // tamper after signing
    std::string json = makeTokenJson(base64Encode(payload.data(), payload.size()), base64Encode(signature, 64));
    check(validator.validate(json, fingerprint) == LicenseStatus::InvalidSignature,
          "tampered payload -> InvalidSignature");
  }

  // --- Malformed JSON (missing fields) ---
  {
    check(validator.validate("{}", fingerprint) == LicenseStatus::NotActivated,
          "missing payload_b64/signature_b64 -> NotActivated");
  }

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
  }
  std::printf("%d test(s) failed.\n", g_failures);
  return EXIT_FAILURE;
}
