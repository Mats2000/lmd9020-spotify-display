#!/bin/bash
# Builds the firmware and sends it to the display over Wi-Fi.
#   tools/ota.sh              finds the display by the broadcast it makes every 2 s
#   tools/ota.sh 192.168.1.82 or names it
# Needs OTA_PASSWORD in include/secrets.h, the visualizer on (VISUALIZER 1), and a firmware
# that takes updates already on the board (the first one goes over USB: pio run -t upload).
# PIO=/path/to/pio to choose pio.
set -euo pipefail
cd "$(dirname "$0")/.."
PIO=${PIO:-pio}

PASS=$(sed -n 's/^#define OTA_PASSWORD "\(.*\)"/\1/p' include/secrets.h)
if [ -z "$PASS" ] || [[ "$PASS" == PASTE* ]]; then
    echo "Set OTA_PASSWORD in include/secrets.h first." >&2
    exit 1
fi
"$PIO" run
LMD_OTA_PASSWORD="$PASS" python3 - "${1:-}" <<'PY'
import hashlib, os, socket, sys, time

ip = sys.argv[1]
if not ip:
    print("Looking for the display…")
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
                ip = addr[0]
                break
    except socket.timeout:
        sys.exit("No display heard on the network. Is it on Wi-Fi? Or pass its address.")
print("Display at", ip)

firmware = open(".pio/build/esp32dev/firmware.bin", "rb").read()
md5 = hashlib.md5(firmware).hexdigest()
token = hashlib.sha256(f"{os.environ['LMD_OTA_PASSWORD']}:{md5}".encode()).hexdigest()

server = socket.socket()
server.bind(("", 0))
server.listen(1)
server.settimeout(1)
port = server.getsockname()[1]
ask = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
request = f"LMDU {len(firmware)} {md5} {token} {port}".encode()
for _ in range(15):  # it answers within a Spotify poll or so
    ask.sendto(request, (ip, 4210))
    try:
        conn, _ = server.accept()
        break
    except socket.timeout:
        continue
else:
    sys.exit("The display didn't come for the update (wrong password, or an older firmware?).")

print(f"Sending {len(firmware) // 1024} KB…")
conn.settimeout(60)  # it erases the slot first, which takes a few seconds
sent, started = 0, time.time()
while sent < len(firmware):
    sent += conn.send(firmware[sent:sent + 4096])
    print(f"\r  {sent * 100 // len(firmware)}%", end="", flush=True)
print()
reply = conn.recv(16).decode(errors="replace").strip()
if reply != "OK":
    sys.exit(f"The display said: {reply or 'nothing'} (see its serial log).")
print(f"Done in {time.time() - started:.0f} s; the display is restarting into it.")
PY
