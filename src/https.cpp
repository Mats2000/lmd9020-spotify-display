#include "https.h"

#include <WiFiClient.h>
#include <esp_system.h>
#include <time.h>

#include "bearssl.h"
#include "trust_anchors.h"

Https https;

namespace {

constexpr uint32_t IO_TIMEOUT_MS = 10000;

WiFiClient tcp;
br_ssl_client_context* sc;
br_x509_minimal_context* xc;
br_sslio_context ioc;
uint8_t* iobuf;

int sockRead(void* ctx, unsigned char* buf, size_t len) {
    WiFiClient* c = (WiFiClient*)ctx;
    uint32_t start = millis();
    for (;;) {
        int avail = c->available();
        if (avail > 0) {
            int n = c->read(buf, len < (size_t)avail ? len : (size_t)avail);
            if (n > 0) return n;
        } else if (!c->connected()) {
            return -1;
        }
        if (millis() - start > IO_TIMEOUT_MS) return -1;
        delay(1);
    }
}

int sockWrite(void* ctx, const unsigned char* buf, size_t len) {
    size_t n = ((WiFiClient*)ctx)->write(buf, len);
    return n > 0 ? (int)n : -1;
}

}  // namespace

bool Https::begin() {
    sc = (br_ssl_client_context*)malloc(sizeof(br_ssl_client_context));
    xc = (br_x509_minimal_context*)malloc(sizeof(br_x509_minimal_context));
    iobuf = (uint8_t*)malloc(BR_SSL_BUFSIZE_MONO);
    if (!sc || !xc || !iobuf) return false;
    br_ssl_client_init_full(sc, xc, TRUST_ANCHORS, TRUST_ANCHORS_NUM);
    // Half-duplex: one buffer, fine for request-then-response HTTP.
    br_ssl_engine_set_buffer(&sc->eng, iobuf, BR_SSL_BUFSIZE_MONO, 0);
    Serial.printf("BearSSL ready: %u bytes\n",
                  (unsigned)(sizeof(br_ssl_client_context) + sizeof(br_x509_minimal_context) + BR_SSL_BUFSIZE_MONO));
    return true;
}

bool Https::connect(const char* host) {
    close();
    if (!tcp.connect(host, 443, 8000)) {
        snprintf(error_, sizeof(error_), "can't connect to %s", host);
        return false;
    }
    tcp.setNoDelay(true);

    // Certificates are checked against today's date: days since 0 AD.
    time_t now = time(nullptr);
    br_x509_minimal_set_time(xc, (uint32_t)(now / 86400 + 719528), (uint32_t)(now % 86400));
    uint8_t seed[32];
    esp_fill_random(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&sc->eng, seed, sizeof(seed));

    // Same host as last time: try to resume the session.
    bool resume = strcmp(sessionHost_, host) == 0;
    if (!br_ssl_client_reset(sc, host, resume)) {
        snprintf(error_, sizeof(error_), "TLS reset failed (%d)", br_ssl_engine_last_error(&sc->eng));
        tcp.stop();
        return false;
    }
    strlcpy(host_, host, sizeof(host_));
    strlcpy(sessionHost_, host, sizeof(sessionHost_));
    br_sslio_init(&ioc, &sc->eng, sockRead, &tcp, sockWrite, &tcp);
    open_ = true;
    return true;
}

void Https::close() {
    if (open_) tcp.stop();
    open_ = false;
    bodyDone_ = true;
}

void Https::drain() {
    uint8_t scratch[128];
    for (int i = 0; i < 400 && !bodyDone_; i++)
        if (read(scratch, sizeof(scratch)) < 0) {
            close();
            return;
        }
    if (!bodyDone_) close();
}

int Https::request(const char* method, const char* host, const char* path, const char* headers,
                   const char* body, size_t bodyLen) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (open_ && !bodyDone_) drain();
        bool reused = open_ && strcmp(host_, host) == 0 && tcp.connected() &&
                      br_ssl_engine_current_state(&sc->eng) != BR_SSL_CLOSED;
        if (!reused && !connect(host)) return -1;

        char head[768];
        int n = snprintf(head, sizeof(head),
                         "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: lmd9020-display\r\n"
                         "Connection: keep-alive\r\n%s",
                         method, path, host, headers);
        if (body) n += snprintf(head + n, sizeof(head) - n, "Content-Length: %u\r\n", (unsigned)bodyLen);
        n += snprintf(head + n, sizeof(head) - n, "\r\n");
        if (n >= (int)sizeof(head)) {
            strlcpy(error_, "request headers too long", sizeof(error_));
            return -1;
        }

        bool sent = br_sslio_write_all(&ioc, head, n) == 0 &&
                    (!body || br_sslio_write_all(&ioc, body, bodyLen) == 0) && br_sslio_flush(&ioc) == 0;
        int status = sent ? readHeaders() : -1;
        if (status > 0) return status;

        int err = br_ssl_engine_last_error(&sc->eng);
        snprintf(error_, sizeof(error_), "%s %s failed (TLS error %d)", reused ? "kept-alive" : "new",
                 host, err);
        close();
        if (!reused) return -1;
        // A kept-alive connection the server had quietly closed: once more, fresh.
    }
    return -1;
}

int Https::readByte() {
    if (rpos_ >= rlen_) {
        int n = br_sslio_read(&ioc, rbuf_, sizeof(rbuf_));
        if (n <= 0) return -1;
        rlen_ = n;
        rpos_ = 0;
    }
    return rbuf_[rpos_++];
}

bool Https::readLine(char* line, size_t size) {
    size_t n = 0;
    for (;;) {
        int c = readByte();
        if (c < 0) return false;
        if (c == '\n') break;
        if (c != '\r' && n + 1 < size) line[n++] = (char)c;
    }
    line[n] = 0;
    return true;
}

int Https::readHeaders() {
    rpos_ = rlen_ = 0;
    char line[256];
    if (!readLine(line, sizeof(line)) || strncmp(line, "HTTP/1.", 7) != 0) return -1;
    int status = atoi(line + 9);
    bool http11 = line[7] == '1';

    chunked_ = false;
    keepAlive_ = http11;
    contentLength_ = -1;
    retryAfter_ = 0;
    for (;;) {
        if (!readLine(line, sizeof(line))) return -1;
        if (!line[0]) break;
        char* colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        const char* value = colon + 1;
        while (*value == ' ') value++;
        if (!strcasecmp(line, "Content-Length"))
            contentLength_ = atoi(value);
        else if (!strcasecmp(line, "Transfer-Encoding") && strcasestr(value, "chunked"))
            chunked_ = true;
        else if (!strcasecmp(line, "Connection"))
            keepAlive_ = strcasestr(value, "close") == nullptr;
        else if (!strcasecmp(line, "Retry-After"))
            retryAfter_ = (uint32_t)atoi(value);
    }

    firstChunk_ = true;
    remaining_ = chunked_ ? 0 : contentLength_;
    bodyDone_ = status == 204 || status == 304 || (!chunked_ && contentLength_ == 0);
    if (!chunked_ && contentLength_ < 0) keepAlive_ = false;  // body runs to the close
    if (bodyDone_ && !keepAlive_) close();
    return status;
}

int Https::read(uint8_t* buf, size_t len) {
    if (bodyDone_) return 0;
    if (chunked_ && remaining_ == 0) {
        char line[32];
        if (!firstChunk_ && !readLine(line, sizeof(line))) return -1;  // CRLF after the last chunk
        firstChunk_ = false;
        if (!readLine(line, sizeof(line))) return -1;
        remaining_ = strtol(line, nullptr, 16);
        if (remaining_ == 0) {  // last chunk; skip any trailers
            while (readLine(line, sizeof(line)) && line[0]) {
            }
            bodyDone_ = true;
            if (!keepAlive_) close();
            return 0;
        }
    }
    if (remaining_ == 0) {
        bodyDone_ = true;
        if (!keepAlive_) close();
        return 0;
    }
    if (remaining_ > 0 && (long)len > remaining_) len = remaining_;

    int got;
    if (rpos_ < rlen_) {
        got = (int)((rlen_ - rpos_) < len ? (rlen_ - rpos_) : len);
        memcpy(buf, rbuf_ + rpos_, got);
        rpos_ += got;
    } else {
        got = br_sslio_read(&ioc, buf, len);
        if (got <= 0) {
            if (remaining_ < 0) {  // body ran to the close: that's the end
                bodyDone_ = true;
                close();
                return 0;
            }
            return -1;
        }
    }
    if (remaining_ > 0) remaining_ -= got;
    return got;
}

bool Https::readString(String& out, size_t maxLen) {
    out = "";
    if (contentLength_ > 0) out.reserve(contentLength_);
    uint8_t buf[256];
    for (;;) {
        int n = read(buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) return true;
        if (out.length() + n > maxLen) return false;
        out.concat((const char*)buf, n);
    }
}

bool splitUrl(const String& url, String& host, String& path) {
    if (!url.startsWith("https://")) return false;
    int slash = url.indexOf('/', 8);
    host = slash < 0 ? url.substring(8) : url.substring(8, slash);
    path = slash < 0 ? String("/") : url.substring(slash);
    return host.length() > 0;
}
