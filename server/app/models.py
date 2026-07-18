"""Pydantic schemas mirroring the token envelope in ARCHITECTURE.md §6."""

from pydantic import BaseModel


class ActivateRequest(BaseModel):
    deviceFingerprint: str
    licenseKey: str


class LicenseToken(BaseModel):
    license_id: str
    device_fingerprint_hash: str
    issued_at: int
    expires_at: int
    plan: str
    sig_alg: str
    key_id: str
    payload_b64: str
    signature_b64: str


class ValidateRequest(BaseModel):
    token: LicenseToken
    deviceFingerprint: str


class ValidateResponse(BaseModel):
    status: str
