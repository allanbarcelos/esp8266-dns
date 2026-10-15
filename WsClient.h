// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>

// WebSocket client (RFC 6455) over a connection the caller already opened
// (TLS, TlsClient.h), for the remote access relay (RelayService.h). Only
// what the relay uses: each message in a single frame, binary or text,
// ping/pong and close. The server is the relay's own Worker, reached over
// TLS with its certificate checked: Sec-WebSocket-Accept is not checked
// (TLS already says who answers, and SHA-1 + base64 would cost flash).
//
// Frames the device sends are masked, as the RFC requires of a client.
// A frame from the server that is masked, fragmented, reserved or longer
// than MAX_FRAME is a protocol error: the caller closes the connection.
class WsClient {
public:
    enum : uint8_t { TEXT = 1, BINARY = 2, CLOSE = 8, PING = 9, PONG = 10 };
    static constexpr uint32_t      MAX_FRAME = 8192;
    static constexpr unsigned long BYTE_MS   = 2000;   // each byte once a frame started

    explicit WsClient(WiFiClient& c) : _c(c) {}

    // The upgrade request on the open connection; extra: header lines, each
    // ending in \r\n. Returns the HTTP status (101 = open) or -1.
    int handshake(const char* host, const char* path, const char* extra) {
        _left = 0;
        _error = false;
        static const char REQ[] PROGMEM =
            " HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ";
        char key[25];
        _key(key);
        _c.print("GET ");
        _c.print(path);
        _c.write_P(REQ, sizeof(REQ) - 1);
        _c.print(key);
        _c.print("\r\nHost: ");
        _c.print(host);
        _c.print("\r\n");
        _c.print(extra);
        _c.print("\r\n");

        int code = -1;
        char line[48];
        for (bool status = true;; status = false) {
            int n = _line(line, sizeof(line));
            if (n < 0) return -1;
            if (status) {
                if (strncmp(line, "HTTP/1.1 ", 9) != 0) return -1;
                code = atoi(line + 9);
            } else if (n == 0) {
                return code;
            }
        }
    }

    // One frame, masked: head then body, as a single message
    void send(uint8_t op, const void* head, size_t nh, const void* body = nullptr, size_t nb = 0) {
        size_t n = nh + nb;
        uint8_t h[8];
        size_t k = 0;
        h[k++] = (uint8_t)(0x80 | op);
        if (n < 126) {
            h[k++] = (uint8_t)(0x80 | n);
        } else {
            h[k++] = 0x80 | 126;
            h[k++] = (uint8_t)(n >> 8);
            h[k++] = (uint8_t)n;
        }
        uint32_t r = ESP.random();
        const uint8_t mask[4] = {(uint8_t)r, (uint8_t)(r >> 8), (uint8_t)(r >> 16), (uint8_t)(r >> 24)};
        memcpy(h + k, mask, 4);
        _c.write(h, k + 4);
        size_t at = 0;
        _masked(static_cast<const uint8_t*>(head), nh, mask, at);
        _masked(static_cast<const uint8_t*>(body), nb, mask, at);
    }

    // A frame header, if one has started arriving. true with op() and
    // left() set; false with nothing yet. A broken frame sets error().
    bool frame() {
        if (_c.available() < 2) return false;
        uint8_t b0 = (uint8_t)_c.read();
        uint8_t b1 = (uint8_t)_c.read();
        _op = b0 & 0x0F;
        uint32_t n = b1 & 0x7F;
        if (n >= 126) {
            n = 0;
            for (int i = 0, bytes = (b1 & 0x7F) == 126 ? 2 : 8; i < bytes; i++) {
                int c = getc();
                if (c < 0) return _fail();
                if (n > (MAX_FRAME >> 8)) return _fail();
                n = (n << 8) | (uint8_t)c;
            }
        }
        _left = n;
        // FIN set, no reserved bits, not masked, not a continuation, not too long
        if ((b0 & 0xF0) != 0x80 || (b1 & 0x80) || _op == 0 || n > MAX_FRAME) return _fail();
        return true;
    }

    uint8_t  op() const    { return _op; }
    uint32_t left() const  { return _left; }
    bool     error() const { return _error; }

    // One byte of the frame (waits up to BYTE_MS); -1 at its end or on a timeout
    int getc() {
        unsigned long t = millis();
        for (;;) {
            if (_c.available() > 0) return _c.read();
            if (!_c.connected() || millis() - t >= BYTE_MS) return -1;
            delay(1);
        }
    }

    // A byte of the payload, counted; -1 at its end or on a timeout
    int next() {
        if (!_left) return -1;
        int c = getc();
        if (c < 0) {
            _error = true;
            return -1;
        }
        _left--;
        return c;
    }

    // Up to cap - 1 bytes of the payload into buf, the rest skipped; the length kept
    size_t read(char* buf, size_t cap) {
        size_t n = 0;
        int c;
        while ((c = next()) >= 0)
            if (n + 1 < cap) buf[n++] = (char)c;
        buf[n] = '\0';
        return n;
    }

    void skip() {
        while (next() >= 0) {}
    }

private:
    WiFiClient& _c;
    uint8_t     _op = 0;
    uint32_t    _left = 0;
    bool        _error = false;

    bool _fail() {
        _error = true;
        return false;
    }

    void _masked(const uint8_t* p, size_t n, const uint8_t* mask, size_t& at) {
        uint8_t buf[64];
        while (n) {
            size_t k = n < sizeof(buf) ? n : sizeof(buf);
            for (size_t i = 0; i < k; i++, at++) buf[i] = p[i] ^ mask[at & 3];
            _c.write(buf, k);
            p += k;
            n -= k;
        }
    }

    // A line without the \r\n (the excess dropped); its length, or -1
    int _line(char* buf, size_t cap) {
        size_t n = 0;
        for (;;) {
            int c = getc();
            if (c < 0) return -1;
            if (c == '\n') break;
            if (c != '\r' && n + 1 < cap) buf[n++] = (char)c;
        }
        buf[n] = '\0';
        return (int)n;
    }

    // 16 random bytes in base64 (24 characters)
    static void _key(char* out) {
        static const char B64[] PROGMEM =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        uint8_t raw[18] = {};
        for (int i = 0; i < 16; i += 4) {
            uint32_t r = ESP.random();
            memcpy(raw + i, &r, 4);
        }
        for (int i = 0, o = 0; i < 18; i += 3) {
            uint32_t v = (uint32_t)raw[i] << 16 | (uint32_t)raw[i + 1] << 8 | raw[i + 2];
            for (int j = 3; j >= 0; j--) out[o++] = (char)pgm_read_byte(&B64[(v >> (6 * j)) & 63]);
        }
        out[22] = out[23] = '=';
        out[24] = '\0';
    }
};
