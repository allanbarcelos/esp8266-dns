// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
// WebSocket frames on the test side: what a server sends (unmasked) and
// what the device sent (masked), decoded.
#include <string>
#include <cstdint>

// A server frame; long64 forces the 64-bit length form
inline std::string serverFrame(uint8_t op, const std::string& payload, bool long64 = false) {
    std::string f;
    f += (char)(0x80 | op);
    size_t n = payload.size();
    if (long64) {
        f += (char)127;
        for (int i = 7; i >= 0; i--) f += (char)((uint64_t)n >> (8 * i));
    } else if (n < 126) {
        f += (char)n;
    } else {
        f += (char)126;
        f += (char)(n >> 8);
        f += (char)n;
    }
    return f + payload;
}

struct ClientFrame {
    bool        ok = false;
    bool        masked = false;
    uint8_t     op = 0;
    std::string payload;
};

// The next frame the device wrote, from at; at moves past it
inline ClientFrame takeClientFrame(const std::string& wire, size_t& at) {
    ClientFrame f;
    if (wire.size() < at + 2) return f;
    uint8_t b0 = (uint8_t)wire[at], b1 = (uint8_t)wire[at + 1];
    size_t p = at + 2;
    size_t n = b1 & 0x7F;
    if (n == 126) {
        if (wire.size() < p + 2) return f;
        n = ((uint8_t)wire[p] << 8) | (uint8_t)wire[p + 1];
        p += 2;
    }
    f.masked = (b1 & 0x80) != 0;
    uint8_t mask[4] = {};
    if (f.masked) {
        if (wire.size() < p + 4) return f;
        for (int i = 0; i < 4; i++) mask[i] = (uint8_t)wire[p + i];
        p += 4;
    }
    if (wire.size() < p + n) return f;
    for (size_t i = 0; i < n; i++) f.payload += (char)((uint8_t)wire[p + i] ^ mask[i & 3]);
    f.op = b0 & 0x0F;
    f.ok = (b0 & 0x80) != 0;
    at = p + n;
    return f;
}
