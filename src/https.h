#pragma once

#include <Arduino.h>

// HTTPS over BearSSL (from the SSLClient library), one kept-alive connection at a time.
// Far less RAM than the core's mbedTLS client, which doesn't fit beside the framebuffers.
class Https {
public:
    bool begin();

    // Sends a request and reads the headers. Returns the HTTP status or -1 (see lastError()).
    int request(const char* method, const char* host, const char* path, const char* headers = "",
                const char* body = nullptr, size_t bodyLen = 0);

    // The response body, after request(): bytes read, 0 at the end, -1 on error.
    int read(uint8_t* buf, size_t len);
    bool readString(String& out, size_t maxLen = 32768);
    int contentLength() const { return contentLength_; }
    uint32_t retryAfterS() const { return retryAfter_; }

    void close();
    const char* lastError() const { return error_; }

private:
    bool connect(const char* host);
    int readHeaders();
    int readByte();
    bool readLine(char* line, size_t size);
    void drain();

    char host_[64] = "";
    char sessionHost_[64] = "";
    bool open_ = false;

    uint8_t rbuf_[512];
    size_t rpos_ = 0, rlen_ = 0;

    bool chunked_ = false, keepAlive_ = false, bodyDone_ = true, firstChunk_ = true;
    long remaining_ = -1;  // bytes left in the body (or current chunk); -1 = until close
    int contentLength_ = -1;
    uint32_t retryAfter_ = 0;
    char error_[64] = "";
};

extern Https https;

// The response body as an ArduinoJson reader: a reply is parsed as it arrives instead of
// being held whole in RAM first (Spotify's are several KB, mostly lists of markets).
class HttpsBodyReader {
public:
    int read() {
        if (pos_ == len_) {
            const int n = https.read(buf_, sizeof(buf_));
            if (n <= 0) return -1;
            pos_ = 0, len_ = n;
        }
        return buf_[pos_++];
    }
    size_t readBytes(char* out, size_t n) {
        size_t got = 0;
        for (int c; got < n && (c = read()) >= 0;) out[got++] = (char)c;
        return got;
    }

private:
    uint8_t buf_[128];
    int pos_ = 0, len_ = 0;
};

// Splits "https://host/path" into its parts. False if it isn't one.
bool splitUrl(const String& url, String& host, String& path);
