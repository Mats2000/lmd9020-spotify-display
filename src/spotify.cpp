#include "log.h"
#include "spotify.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <base64.h>

#include "config.h"
#include "https.h"
#include "render/art.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and fill it in (see README)."
#endif

static const char* ACCOUNTS_HOST = "accounts.spotify.com";
static const char* API_HOST = "api.spotify.com";
static const char* CURRENT_PATH = "/v1/me/player?additional_types=track,episode";  // also says which device

static bool deviceShown(const String& type) {
    String list = "," SPOTIFY_DEVICES ",";
    return list == ",," || list.indexOf("," + type + ",") >= 0;
}

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
    if (!https.begin()) say("%s\n", "Out of memory for TLS");

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
        say("Spotify token refresh failed: HTTP %d %s %s\n", code, code < 0 ? https.lastError() : "",
                      body.c_str());
        // 400/401: bad credentials or token; anything else is retried.
        if (code == 400 || code == 401) failure = SpotifyResult::AuthError;
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["access_token"].is<const char*>()) {
        say("%s\n", "Spotify token refresh: unexpected response");
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
        say("%s\n", "Spotify issued a new refresh token; saved");
    }
    say("%s\n", "Spotify access token refreshed");
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

// The image below `width` and still at least the cover's height on screen.
static String pickSmall(JsonArrayConst images, int width, int& bestW) {
    String best;
    bestW = 0;
    for (JsonObjectConst img : images) {
        int w = img["width"] | 0;
        const char* url = img["url"];
        if (!url || w >= width || w < ART_H || w <= bestW) continue;
        best = url;
        bestW = w;
    }
    return best;
}

static void itemFilter(JsonObject f) {
    f["uri"] = true;
    f["name"] = true;
    f["duration_ms"] = true;
    f["artists"][0]["name"] = true;
    f["album"]["images"][0]["url"] = true;
    f["album"]["images"][0]["width"] = true;
    f["images"][0]["url"] = true;
    f["images"][0]["width"] = true;
    f["show"]["name"] = true;
}

static void fillTrack(JsonObjectConst item, Track& track) {
    track.title = item["name"] | "";
    track.artist = "";
    track.durationMs = item["duration_ms"] | 0;
    JsonArrayConst images;
    if (item["show"].is<JsonObjectConst>()) {  // podcast episode
        track.artist = item["show"]["name"] | "";
        images = item["images"];
    } else {
        for (JsonObjectConst a : item["artists"].as<JsonArrayConst>()) {
            if (track.artist.length()) track.artist += ", ";
            track.artist += a["name"] | "";
        }
        images = item["album"]["images"];
    }
    track.imageUrl = pickImage(images, track.imageWidth);
    track.thumbUrl = pickThumb(images, track.thumbWidth);
    track.smallUrl = pickSmall(images, track.imageWidth, track.smallWidth);
    track.key = item["uri"] | "";
    if (track.key.isEmpty()) track.key = track.title + "\n" + track.artist;
}

bool SpotifyClient::tokenReady(SpotifyResult& failure) {
    if (!accessToken_.isEmpty() && (int32_t)(millis() - tokenExpiry_) < 0) return true;
    return refreshAccessToken(failure);
}

SpotifyResult SpotifyClient::current(Track& track, uint32_t& retryAfterMs) {
    SpotifyResult failure;
    if (!tokenReady(failure)) return failure;

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
        say("Spotify rate limit, waiting %u s\n", retryAfterMs / 1000);
        return SpotifyResult::RateLimited;
    }
    if (code != 200) {
        say("Spotify currently-playing: HTTP %d %s\n", code, code < 0 ? https.lastError() : "");
        return SpotifyResult::NetError;
    }
    // Keep only what the display uses, parsing as the reply arrives.
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["progress_ms"] = true;
    filter["device"]["type"] = true;
    filter["device"]["name"] = true;
    itemFilter(filter["item"].to<JsonObject>());

    JsonDocument doc;
    HttpsBodyReader body;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        say("Spotify JSON: %s\n", err.c_str());
        https.close();
        return SpotifyResult::NetError;
    }
    String type = doc["device"]["type"] | "";
    static String lastDevice;
    String device = type + " " + (doc["device"]["name"] | "");
    if (device != lastDevice) {
        lastDevice = device;
        say("Spotify on %s%s\n", device.c_str(), deviceShown(type) ? "" : " (not shown)");
    }
    if (!deviceShown(type)) return SpotifyResult::Nothing;

    JsonObjectConst item = doc["item"];
    if (item.isNull()) return SpotifyResult::Nothing;  // e.g. a private session

    track.playing = doc["is_playing"] | false;
    track.progressMs = doc["progress_ms"] | 0;
    fillTrack(item, track);
    return SpotifyResult::Item;
}

// Reads up to the top-level "queue" array's opening bracket. The reply holds up to 20 whole
// songs; only the first is parsed, the rest is never read.
static bool skipToQueue(HttpsBodyReader& body) {
    int depth = 0;
    bool inString = false, escaped = false;
    char key[8];
    int keyLen = -1;  // >= 0 while reading a string at depth 1
    bool afterKey = false;
    for (int c; (c = body.read()) >= 0;) {
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') {
                inString = false;
                afterKey = keyLen == 5 && memcmp(key, "queue", 5) == 0;
                keyLen = -1;
            } else if (keyLen >= 0) {
                if (keyLen < (int)sizeof(key)) key[keyLen] = (char)c;
                keyLen++;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            keyLen = depth == 1 ? 0 : -1;
        } else if (c == '{' || c == '[') {
            if (afterKey && depth == 1 && c == '[') return true;
            depth++;
        } else if (c == '}' || c == ']') {
            depth--;
        } else if (c != ':' && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            afterKey = false;
        }
    }
    return false;
}

SpotifyResult SpotifyClient::next(Track& track) {
    SpotifyResult failure;
    if (!tokenReady(failure)) return failure;
    String headers = "Authorization: Bearer " + accessToken_ + "\r\n";
    int code = https.request("GET", API_HOST, "/v1/me/player/queue", headers.c_str());
    if (code == 204) return SpotifyResult::Nothing;
    if (code == 401) accessToken_ = "";
    if (code != 200) {
        say("Spotify queue: HTTP %d %s\n", code, code < 0 ? https.lastError() : "");
        if (code == 429) https.close();
        return code == 429 ? SpotifyResult::RateLimited : SpotifyResult::NetError;
    }
    HttpsBodyReader body;
    SpotifyResult result = SpotifyResult::Nothing;
    if (skipToQueue(body)) {
        JsonDocument filter;
        itemFilter(filter.to<JsonObject>());
        JsonDocument doc;
        if (!deserializeJson(doc, body, DeserializationOption::Filter(filter)) && doc.is<JsonObjectConst>()) {
            fillTrack(doc.as<JsonObjectConst>(), track);
            result = SpotifyResult::Item;
        }
    }
    https.close();
    return result;
}
