#pragma once

// Copy this file to secrets.h (which git ignores) and fill it in.
// tools/spotify_auth.py fills in the three Spotify values for you.

#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"

// From your app at developer.spotify.com/dashboard
#define SPOTIFY_CLIENT_ID "PASTE_CLIENT_ID"
#define SPOTIFY_CLIENT_SECRET "PASTE_CLIENT_SECRET"
// From tools/spotify_auth.py
#define SPOTIFY_REFRESH_TOKEN "PASTE_REFRESH_TOKEN"

// Any password: updates over Wi-Fi (tools/ota.sh) need it. Leave as is to turn them off.
#define OTA_PASSWORD "PASTE a password"
