#pragma once

#include <Arduino.h>

struct Track {
    String key;       // changes whenever the item changes (its Spotify URI)
    String title;
    String artist;    // all artists, comma-separated; the show for podcasts
    String imageUrl;  // empty for local files
    int imageWidth = 0;
    String thumbUrl;  // the smallest size, for picking the cover's colours
    int thumbWidth = 0;
    String smallUrl;  // the next size down from imageUrl, when that won't fit the prefetch slot
    int smallWidth = 0;
    bool playing = false;
    uint32_t progressMs = 0, durationMs = 0;
};

enum class SpotifyResult {
    Item,         // `track` is filled in (playing or paused)
    Nothing,      // no active device or nothing loaded
    AuthError,    // refresh token rejected: rerun tools/spotify_auth.py
    RateLimited,  // back off for `retryAfterMs`
    NetError,     // transient; try again shortly
};

// Spotify Web API: token refresh and currently-playing on a kept-alive connection.
class SpotifyClient {
public:
    void begin();
    SpotifyResult current(Track& track, uint32_t& retryAfterMs);
    // The first song in the queue (title, artist, covers). Closes the connection after it.
    SpotifyResult next(Track& track);
    // Closes the kept-alive connection.
    void closeConnection();

private:
    // On failure returns false and sets `failure` to AuthError or NetError.
    bool refreshAccessToken(SpotifyResult& failure);
    bool tokenReady(SpotifyResult& failure);

    String refreshToken_;
    String accessToken_;
    uint32_t tokenExpiry_ = 0;
};
