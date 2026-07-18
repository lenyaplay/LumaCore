from fastapi.testclient import TestClient

from app.main import DEMO_LICENSE_KEYS, app

client = TestClient(app)
DEMO_KEY = next(iter(DEMO_LICENSE_KEYS))


def _activate(device_fingerprint: str, license_key: str = DEMO_KEY):
    return client.post("/activate", json={"deviceFingerprint": device_fingerprint, "licenseKey": license_key})


def test_activate_unknown_key_rejected():
    response = _activate("device-1", license_key="NOPE")
    assert response.status_code == 403


def test_activate_then_validate_round_trip():
    token = _activate("device-1").json()

    response = client.post("/validate", json={"token": token, "deviceFingerprint": "device-1"})
    assert response.json()["status"] == "VALID"


def test_validate_rejects_wrong_device():
    token = _activate("device-1").json()

    response = client.post("/validate", json={"token": token, "deviceFingerprint": "device-2"})
    assert response.json()["status"] == "DEVICE_MISMATCH"


def test_validate_rejects_tampered_signature():
    token = _activate("device-1").json()
    token["signature_b64"] = token["signature_b64"][:-4] + "AAAA"

    response = client.post("/validate", json={"token": token, "deviceFingerprint": "device-1"})
    assert response.json()["status"] == "INVALID_SIGNATURE"
