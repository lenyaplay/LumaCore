#!/usr/bin/env bash
# Runs the mock license backend (server/app/main.py) so physical devices on
# the same LAN (not just this Mac) can reach it — see
# lib/core/license/license_config.dart for the LICENSE_SERVER_URL override
# a physical iOS/Android device needs to point at this Mac's LAN IP instead
# of 127.0.0.1.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi
source .venv/bin/activate
pip install -q -r requirements.txt

LAN_IP="$(ipconfig getifaddr en0 2>/dev/null || echo "<could not detect — check Wi-Fi is connected>")"
echo "=== Mock license server ===" >&2
echo "Simulator / same-Mac clients: http://127.0.0.1:8000" >&2
echo "Physical device on this LAN:  http://${LAN_IP}:8000" >&2
echo "  flutter run --dart-define=LICENSE_SERVER_URL=http://${LAN_IP}:8000" >&2
echo "===========================" >&2

# --host 0.0.0.0: without this, uvicorn only listens on localhost and no
# physical device (iOS or Android) can reach it regardless of the LAN IP
# above.
exec python3 -m uvicorn app.main:app --host 0.0.0.0 --port 8000
