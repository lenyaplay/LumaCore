#ifndef LUMACORE_SHA256_H
#define LUMACORE_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// FIPS 180-4 SHA-256, one-shot. outHash must be 32 bytes.
void sha256(const uint8_t* data, size_t len, uint8_t outHash[32]);

#ifdef __cplusplus
}
#endif

#endif  // LUMACORE_SHA256_H
