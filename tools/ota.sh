#!/bin/bash
# Builds the firmware and sends it to the display over Wi-Fi.
#   tools/ota.sh              finds the display by the broadcast it makes every 2 s
#   tools/ota.sh 192.168.1.82 or names it
# Needs OTA_PASSWORD in include/secrets.h, and a firmware with updates over Wi-Fi already on
# the board (the first one goes over USB: pio run -t upload). PIO=/path/to/pio to choose pio.
set -euo pipefail
cd "$(dirname "$0")/.."
PIO=${PIO:-pio}

PASS=$(sed -n 's/^#define OTA_PASSWORD "\(.*\)"/\1/p' include/secrets.h)
if [ -z "$PASS" ] || [[ "$PASS" == PASTE* ]]; then
    echo "Set OTA_PASSWORD in include/secrets.h first." >&2
    exit 1
fi

IP=${1:-}
if [ -z "$IP" ]; then
    echo "Looking for the display…"
    IP=$(python3 - <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
if hasattr(socket, "SO_REUSEPORT"):
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
s.bind(("", 4211))
s.settimeout(6)
try:
    while True:
        data, addr = s.recvfrom(64)
        if data.startswith(b"LMD9020"):
            print(addr[0])
            break
except socket.timeout:
    sys.exit(1)
PY
) || { echo "No display heard on the network. Is it on Wi-Fi? Or pass its address." >&2; exit 1; }
fi
echo "Display at $IP"
LMD_OTA_PASSWORD="$PASS" "$PIO" run -e ota -t upload --upload-port "$IP"
