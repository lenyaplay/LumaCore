"""Ed25519 sign/verify helpers for the mock license backend.

ARCHITECTURE.md §6: Ed25519 (libsodium/PyNaCl) instead of ECDSA — libsodium
gives EdDSA natively, no ASN.1 signature encoding, equivalent security.
"""

import base64
import struct
import uuid
from pathlib import Path

import nacl.exceptions
import nacl.signing

# Dev-only keypair, persisted to a gitignored seed file so it survives server
# restarts. Without this, the public key native/src/api/lumacore_api.cpp
# hardcodes (fetched once from GET /public-key) goes stale on every restart —
# TokenValidator would reject every previously-issued token. Only the public
# key needs to be shared with a client (embedded in the native binary in the
# real app); the private key/seed never leaves this mock server.
_SEED_PATH = Path(__file__).resolve().parent.parent / ".dev_signing_key"

if _SEED_PATH.exists():
    _signing_key = nacl.signing.SigningKey(_SEED_PATH.read_bytes())
else:
    _signing_key = nacl.signing.SigningKey.generate()
    _SEED_PATH.write_bytes(bytes(_signing_key))
    _SEED_PATH.chmod(0o600)

_verify_key = _signing_key.verify_key

SIG_ALG = "ed25519"
KEY_ID = "v1"


def public_key_b64() -> str:
    return base64.b64encode(bytes(_verify_key)).decode("ascii")


def sign_payload(payload: bytes) -> str:
    signature = _signing_key.sign(payload).signature
    return base64.b64encode(signature).decode("ascii")


def verify_payload(payload: bytes, signature_b64: str) -> bool:
    try:
        signature = base64.b64decode(signature_b64)
        _verify_key.verify(payload, signature)
        return True
    except (nacl.exceptions.BadSignatureError, ValueError):
        return False


def encode_payload(
    license_id: str,
    device_fingerprint_hash: bytes,
    issued_at: int,
    expires_at: int,
    plan: str,
) -> bytes:
    """Canonical binary payload that gets signed — not raw JSON, since JSON
    canonicalization is a classic verification pitfall (ARCHITECTURE.md §6).
    license_id(16) + fingerprint_hash(32) + issued_at(8) + expires_at(8) +
    plan_len(1) + plan(utf-8).
    """
    plan_bytes = plan.encode("utf-8")
    return (
        uuid.UUID(license_id).bytes
        + device_fingerprint_hash
        + struct.pack(">QQ", issued_at, expires_at)
        + struct.pack(">B", len(plan_bytes))
        + plan_bytes
    )
