"""Mock license activation backend (ARCHITECTURE.md §6, Task.md §8).

Real cryptography (Ed25519 via PyNaCl), no production environment — this
exists to give the client a REST contract to activate/validate against, and
to demonstrate the same signature scheme the C++ TokenValidator will verify
offline in Этап 6.
"""

import base64
import hashlib
import time
import uuid

from fastapi import FastAPI, HTTPException

from . import crypto
from .models import ActivateRequest, LicenseToken, ValidateRequest, ValidateResponse

app = FastAPI(title="LumaCore mock license backend")

# Demo license-key table, in memory only. A real deployment would look this
# up in a database — explicitly out of scope (Task.md §8).
DEMO_LICENSE_KEYS = {
    "DEMO-0000-0000": "portfolio-demo",
}

TOKEN_VALIDITY_SECONDS = 365 * 24 * 3600


@app.get("/public-key")
def public_key() -> dict:
    return {"sig_alg": crypto.SIG_ALG, "key_id": crypto.KEY_ID, "public_key_b64": crypto.public_key_b64()}


@app.post("/activate", response_model=LicenseToken)
def activate(req: ActivateRequest) -> LicenseToken:
    plan = DEMO_LICENSE_KEYS.get(req.licenseKey)
    if plan is None:
        raise HTTPException(status_code=403, detail="unknown license key")

    license_id = str(uuid.uuid4())
    fingerprint_hash = hashlib.sha256(req.deviceFingerprint.encode("utf-8")).digest()
    issued_at = int(time.time())
    expires_at = issued_at + TOKEN_VALIDITY_SECONDS

    payload = crypto.encode_payload(license_id, fingerprint_hash, issued_at, expires_at, plan)
    signature_b64 = crypto.sign_payload(payload)

    return LicenseToken(
        license_id=license_id,
        device_fingerprint_hash=fingerprint_hash.hex(),
        issued_at=issued_at,
        expires_at=expires_at,
        plan=plan,
        sig_alg=crypto.SIG_ALG,
        key_id=crypto.KEY_ID,
        payload_b64=base64.b64encode(payload).decode("ascii"),
        signature_b64=signature_b64,
    )


@app.post("/validate", response_model=ValidateResponse)
def validate(req: ValidateRequest) -> ValidateResponse:
    token = req.token
    payload = base64.b64decode(token.payload_b64)

    if not crypto.verify_payload(payload, token.signature_b64):
        return ValidateResponse(status="INVALID_SIGNATURE")

    expected_hash = hashlib.sha256(req.deviceFingerprint.encode("utf-8")).hexdigest()
    if expected_hash != token.device_fingerprint_hash:
        return ValidateResponse(status="DEVICE_MISMATCH")

    if int(time.time()) > token.expires_at:
        return ValidateResponse(status="EXPIRED")

    return ValidateResponse(status="VALID")
