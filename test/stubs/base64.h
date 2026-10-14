// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "Arduino.h"

// The core's base64 (no line breaks), for real: the test checks the
// Authorization header a browser would send
class base64 {
public:
    static String encode(const String& text) {
        static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const std::string& in = text._s;
        std::string out;
        for (size_t i = 0; i < in.size(); i += 3) {
            uint32_t v = (uint8_t)in[i] << 16;
            if (i + 1 < in.size()) v |= (uint8_t)in[i + 1] << 8;
            if (i + 2 < in.size()) v |= (uint8_t)in[i + 2];
            out += T[v >> 18 & 63];
            out += T[v >> 12 & 63];
            out += i + 1 < in.size() ? T[v >> 6 & 63] : '=';
            out += i + 2 < in.size() ? T[v & 63] : '=';
        }
        return String(out.c_str());
    }
};
