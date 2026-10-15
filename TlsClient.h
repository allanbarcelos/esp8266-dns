// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>

// WiFiClientSecure's connect uses the BearSSL context timeout (_owned).
// Setting only the wrapper's timeout leaves the context with the core's
// 15 s: each unreachable host would block the loop for that long.
class TlsClient : public WiFiClientSecure {
public:
    // BearSSL's default buffer is 16 KB. This device has ~30 KB of heap
    // left: that allocation fragments memory, the watchdog restarts the ESP
    // and, without a contiguous block, the handshake raises an Exception.
    static constexpr uint16_t RX_BUF = 4096;
    static constexpr uint16_t TX_BUF = 512;

    // Only ECDHE with AES-GCM, which every server in use accepts
    // (Cloudflare, GitHub, ntfy.sh, ipify, icanhazip, ifconfig.co). The other
    // ciphers are not even in the firmware (TlsTrim.h): announcing one would
    // let the server pick something the device cannot do.
    static constexpr int SUITE_COUNT = 4;

    bool prepare(unsigned long ms, uint16_t rx = RX_BUF, uint16_t tx = TX_BUF) {
        if (!_owned) return false;
        setTimeout(ms);
        _owned->setTimeout(ms);
        setBufferSizes(rx, tx);
        return setCiphers(_SUITES, SUITE_COUNT);
    }

private:
    // ECDHE_ECDSA and ECDHE_RSA, AES_128_GCM_SHA256 and AES_256_GCM_SHA384
    static constexpr uint16_t _SUITES[SUITE_COUNT] = {0xC02B, 0xC02F, 0xC02C, 0xC030};
};

// One HTTPS request on a prepared TlsClient (roots, clock, buffers), in
// place of the core's HTTPClient (~6 KB): the device only ever sends one
// request per connection and reads a short answer. HTTP/1.0, so the server
// answers without chunked encoding and closes the connection; the body ends
// there or at its Content-Length.
namespace https {

constexpr int NO_CONNECTION = -1;   // TCP or TLS did not open
constexpr int BAD_RESPONSE  = -2;   // no status line, or it stopped halfway

// A byte of the answer, or -1 when the connection closed or ms passed with nothing
inline int _getc(WiFiClient& c, unsigned long& last, unsigned long ms) {
    for (;;) {
        if (c.available() > 0) {
            last = millis();
            return c.read();
        }
        if (!c.connected() || millis() - last >= ms) return -1;
        delay(1);
    }
}

// A piece of the body: in RAM, or in flash (pgm)
struct Part {
    const char* p;
    size_t      n;
    bool        pgm;
};

// Gets the answer's body one byte at a time
using Sink = void (*)(void* ctx, char c);

// headers: extra lines, each ending in \r\n (may be null). The body goes
// out in parts (count may be 0); the answer's body, if sink is set, goes
// to it as it arrives. Returns the HTTP status, NO_CONNECTION or
// BAD_RESPONSE; the connection is closed.
inline int exchange(WiFiClient& c, const char* host, const char* method, const char* path,
                    const char* headers, const Part* parts, size_t count, unsigned long ms,
                    Sink sink = nullptr, void* ctx = nullptr) {
    if (!c.connect(host, 443)) return NO_CONNECTION;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) total += parts[i].n;
    char len[40] = "";
    if (count) snprintf_P(len, sizeof(len), PSTR("Content-Length: %u\r\n"), (unsigned)total);
    // Fixed text from flash: free heap is what TLS needs
    static const char HTTP10[] PROGMEM = " HTTP/1.0\r\nHost: ";
    static const char TAIL[] PROGMEM = "\r\nUser-Agent: ESP8266\r\nConnection: close\r\n";
    c.print(method);
    c.print(" ");
    c.print(path);
    c.write_P(HTTP10, sizeof(HTTP10) - 1);
    c.print(host);
    c.write_P(TAIL, sizeof(TAIL) - 1);
    if (headers) c.print(headers);
    c.print(len);
    c.print("\r\n");
    for (size_t i = 0; i < count; i++) {
        if (parts[i].pgm) c.write_P(parts[i].p, parts[i].n);
        else c.write(reinterpret_cast<const uint8_t*>(parts[i].p), parts[i].n);
    }

    unsigned long last = millis();
    char line[64];
    int code = BAD_RESPONSE;
    long length = -1;
    for (bool status = true;; status = false) {
        size_t n = 0;
        int ch;
        while ((ch = _getc(c, last, ms)) >= 0 && ch != '\n')
            if (ch != '\r' && n + 1 < sizeof(line)) line[n++] = (char)ch;
        line[n] = '\0';
        const char* sp = strchr(line, ' ');
        if (ch < 0 || (status && (strncmp(line, "HTTP/", 5) != 0 || !sp))) {
            c.stop();
            return BAD_RESPONSE;
        }
        if (status) code = atoi(sp + 1);
        else if (n == 0) break;
        else if (strncasecmp(line, "Content-Length:", 15) == 0) length = strtol(line + 15, nullptr, 10);
    }
    int ch;
    for (long got = 0; sink && (length < 0 || got < length) && (ch = _getc(c, last, ms)) >= 0; got++)
        sink(ctx, (char)ch);
    c.stop();
    return code;
}

struct _Text {
    String* out;
    size_t  cap;
};

inline void _keep(void* ctx, char ch) {
    const _Text& t = *static_cast<const _Text*>(ctx);
    if (t.out->length() < t.cap) *t.out += ch;
}

// One request with a body in RAM (may be null). out, if not null, gets up
// to cap bytes of the answer's body.
inline int request(WiFiClient& c, const char* host, const char* method, const char* path,
                   const char* headers, const char* body, unsigned long ms,
                   String* out = nullptr, size_t cap = 2048) {
    Part part{body, body ? strlen(body) : 0, false};
    _Text text{out, cap};
    return exchange(c, host, method, path, headers, &part, body ? 1 : 0, ms,
                    out ? _keep : nullptr, &text);
}

}  // namespace https
