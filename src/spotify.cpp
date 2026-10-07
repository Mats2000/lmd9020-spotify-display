#include "spotify.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <base64.h>

#include "https.h"
#include "render/art.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and fill it in (see README)."
#endif

static const char* ACCOUNTS_HOST = "accounts.spotify.com";
static const char* API_HOST = "api.spotify.com";
static const char* CURRENT_PATH = "/v1/me/player/currently-playing?additional_types=track,episode";

static String urlEncode(const String& s) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[(uint8_t)c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

void SpotifyClient::begin() {
    if (!https.begin()) Serial.println("Out of memory for TLS");

    // The latest refresh token lives in flash (Spotify may rotate it); a new one in secrets.h wins.
    Preferences prefs;
    prefs.begin("spotify", false);
    if (prefs.getString("seed", "") != SPOTIFY_REFRESH_TOKEN) {
        prefs.putString("seed", SPOTIFY_REFRESH_TOKEN);
        prefs.putString("rt", SPOTIFY_REFRESH_TOKEN);
    }
    refreshToken_ = prefs.getString("rt", SPOTIFY_REFRESH_TOKEN);
    prefs.end();
}

void SpotifyClient::closeConnection() { https.close(); }

bool SpotifyClient::refreshAccessToken(SpotifyResult& failure) {
    failure = SpotifyResult::NetError;
    String headers = "Content-Type: application/x-www-form-urlencoded\r\nAuthorization: Basic " +
                     base64::encode(String(SPOTIFY_CLIENT_ID) + ":" + SPOTIFY_CLIENT_SECRET) + "\r\n";
    String form = "grant_type=refresh_token&refresh_token=" + urlEncode(refreshToken_);
    int code = https.request("POST", ACCOUNTS_HOST, "/api/token", headers.c_str(), form.c_str(), form.length());
    String body;
    if (code > 0) https.readString(body, 4096);

    if (code != 200) {
        Serial.printf("Spotify token refresh failed: HTTP %d %s %s\n", code, code < 0 ? https.lastError() : "",
                      body.c_str());
        // 400/401: bad credentials or token; anything else is retried.
        if (code == 400 || code == 401) failure = SpotifyResult::AuthError;
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["access_token"].is<const char*>()) {
        Serial.println("Spotify token refresh: unexpected response");
        return false;
    }
    accessToken_ = doc["access_token"].as<const char*>();
    uint32_t expiresIn = doc["expires_in"] | 3600;
    tokenExpiry_ = millis() + (expiresIn > 300 ? expiresIn - 120 : expiresIn / 2) * 1000UL;

    const char* rotated = doc["refresh_token"];
    if (rotated && refreshToken_ != rotated) {
        refreshToken_ = rotated;
        Preferences prefs;
        prefs.begin("spotify", false);
        prefs.putString("rt", refreshToken_);
        prefs.end();
        Serial.println("Spotify issued a new refresh token; saved");
    }
    Serial.println("Spotify access token refreshed");
    return true;
}

// The biggest cover (640 px): least compressed, and it streams through the decoder.
static String pickThumb(JsonArrayConst images, int& bestW) {
    String best;
    bestW = 0;
    for (JsonObjectConst img : images) {
        int w = img["width"] | 0;
        const char* url = img["url"];
        if (!url || w < 32 || (bestW && w >= bestW)) continue;
        best = url;
        bestW = w;
    }
    return best;
}

static String pickImage(JsonArrayConst images, int& bestW) {
    String best;
    bestW = 0;
    for (JsonObjectConst img : images) {
        int w = img["width"] | 0;
        const char* url = img["url"];
        if (!url || (w > 0 && w <= bestW) || w > 1600) continue;
        best = url;
        bestW = w;
    }
    return best;
}

SpotifyResult SpotifyClient::current(Track& track, uint32_t& retryAfterMs) {
    if (accessToken_.isEmpty() || (int32_t)(millis() - tokenExpiry_) >= 0) {
        SpotifyResult failure;
        if (!refreshAccessToken(failure)) return failure;
    }

    String headers = "Authorization: Bearer " + accessToken_ + "\r\n";
    int code = https.request("GET", API_HOST, CURRENT_PATH, headers.c_str());

    if (code == 204) return SpotifyResult::Nothing;  // nothing playing on any device
    if (code == 401) {  // access token expired early; refresh on the next call
        accessToken_ = "";
        return SpotifyResult::NetError;
    }
    if (code == 429) {
        retryAfterMs = https.retryAfterS() * 1000;
        if (retryAfterMs < 5000) retryAfterMs = 5000;
        Serial.printf("Spotify rate limit, waiting %u s\n", retryAfterMs / 1000);
        return SpotifyResult::RateLimited;
    }
    if (code != 200) {
        Serial.printf("Spotify currently-playing: HTTP %d %s\n", code, code < 0 ? https.lastError() : "");
        return SpotifyResult::NetError;
    }
    // Keep only what the display uses, parsing as the reply arrives.
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["item"]["uri"] = true;
    filter["item"]["name"] = true;
    filter["item"]["artists"][0]["name"] = true;
    filter["item"]["album"]["images"][0]["url"] = true;
    filter["item"]["album"]["images"][0]["width"] = true;
    filter["item"]["images"][0]["url"] = true;
    filter["item"]["images"][0]["width"] = true;
    filter["item"]["show"]["name"] = true;

    JsonDocument doc;
    HttpsBodyReader body;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("Spotify JSON: %s\n", err.c_str());
        https.close();
        return SpotifyResult::NetError;
    }
    JsonObjectConst item = doc["item"];
    if (item.isNull()) return SpotifyResult::Nothing;  // e.g. a private session

    track.playing = doc["is_playing"] | false;
    track.title = item["name"] | "";
    track.artist = "";
    if (item["show"].is<JsonObjectConst>()) {  // podcast episode
        track.artist = item["show"]["name"] | "";
        track.imageUrl = pickImage(item["images"], track.imageWidth);
        track.thumbUrl = pickThumb(item["images"], track.thumbWidth);
    } else {
        for (JsonObjectConst a : item["artists"].as<JsonArrayConst>()) {
            if (track.artist.length()) track.artist += ", ";
            track.artist += a["name"] | "";
        }
        track.imageUrl = pickImage(item["album"]["images"], track.imageWidth);
        track.thumbUrl = pickThumb(item["album"]["images"], track.thumbWidth);
    }
    track.key = item["uri"] | "";
    if (track.key.isEmpty()) track.key = track.title + "\n" + track.artist;
    return SpotifyResult::Item;
}
