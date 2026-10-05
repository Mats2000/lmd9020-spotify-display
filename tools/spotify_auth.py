#!/usr/bin/env python3
"""One-time Spotify login for the display. Standard library only.

Before running, create an app at https://developer.spotify.com/dashboard
(your account needs Premium for apps in Development Mode):
  - Redirect URI:  http://127.0.0.1:8888/callback   (exactly this)
  - API used:      Web API

Then:
    python3 tools/spotify_auth.py

Asks for the Client ID and Secret, opens the consent page, and writes the
refresh token into include/secrets.h.
"""
import base64
import http.server
import json
import os
import re
import secrets
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

PORT = 8888
REDIRECT_URI = f"http://127.0.0.1:{PORT}/callback"
SCOPES = "user-read-currently-playing user-read-playback-state"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECRETS = os.path.join(ROOT, "include", "secrets.h")
EXAMPLE = os.path.join(ROOT, "include", "secrets.example.h")


def read_define(text, name):
    m = re.search(rf'#define\s+{name}\s+"([^"]*)"', text)
    return m.group(1) if m and not m.group(1).startswith("PASTE") else ""


def write_defines(values):
    path = SECRETS if os.path.exists(SECRETS) else EXAMPLE
    with open(path) as f:
        text = f.read()
    for name, value in values.items():
        text, n = re.subn(rf'(#define\s+{name}\s+)"[^"]*"', rf'\g<1>"{value}"', text)
        if not n:
            text += f'\n#define {name} "{value}"\n'
    with open(SECRETS, "w") as f:
        f.write(text)


def ask(prompt, current):
    hint = f" [{current[:6]}…]" if current else ""
    value = input(f"{prompt}{hint}: ").strip()
    return value or current


def wait_for_code(expected_state):
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path != "/callback":
                self.send_error(404)
                return
            q = urllib.parse.parse_qs(url.query)
            result.update({k: v[0] for k, v in q.items()})
            ok = "code" in q and q.get("state", [""])[0] == expected_state
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            msg = ("Done. You can close this tab and go back to the terminal." if ok
                   else "Something went wrong; check the terminal.")
            self.wfile.write(f"<p style='font-family:sans-serif'>{msg}</p>".encode())

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    while "code" not in result and "error" not in result:
        server.handle_request()
    server.server_close()
    if "error" in result:
        sys.exit(f"Spotify said: {result['error']}")
    if result.get("state") != expected_state:
        sys.exit("State mismatch; try again.")
    return result["code"]


def main():
    existing = ""
    if os.path.exists(SECRETS):
        with open(SECRETS) as f:
            existing = f.read()

    print(__doc__.split("Then:")[0].strip(), "\n")
    client_id = ask("Client ID", read_define(existing, "SPOTIFY_CLIENT_ID"))
    client_secret = ask("Client Secret", read_define(existing, "SPOTIFY_CLIENT_SECRET"))
    if not client_id or not client_secret:
        sys.exit("Need both the Client ID and the Client Secret.")

    state = secrets.token_urlsafe(16)
    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "response_type": "code",
        "client_id": client_id,
        "scope": SCOPES,
        "redirect_uri": REDIRECT_URI,
        "state": state,
    })
    print("\nOpening Spotify in your browser. If it doesn't open, visit:\n" + auth_url + "\n")
    webbrowser.open(auth_url)
    code = wait_for_code(state)

    basic = base64.b64encode(f"{client_id}:{client_secret}".encode()).decode()
    req = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode({
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": REDIRECT_URI,
        }).encode(),
        headers={"Authorization": f"Basic {basic}",
                 "Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req) as r:
            tokens = json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"Token exchange failed: {e.code} {e.read().decode()}")
    except urllib.error.URLError as e:
        sys.exit(f"Couldn't reach Spotify: {e.reason}\n(On a python.org install, run "
                 "'Install Certificates.command' from the Python folder first.)")

    write_defines({
        "SPOTIFY_CLIENT_ID": client_id,
        "SPOTIFY_CLIENT_SECRET": client_secret,
        "SPOTIFY_REFRESH_TOKEN": tokens["refresh_token"],
    })
    print(f"Saved to {os.path.relpath(SECRETS, ROOT)}. Now build and flash: pio run -t upload")


if __name__ == "__main__":
    main()
