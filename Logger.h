// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include <stdarg.h>

#define LOG_BUFFER_SIZE 10
#define LOG_LINE_SIZE   128

class Logger {
public:
    Logger() : _head(0), _wrapped(false) {
        memset(_buf, 0, sizeof(_buf));
    }

    // fmt lives in flash (PSTR): on the ESP8266 plain string literals take
    // RAM, and the heap is what the OTA TLS needs. vsnprintf_P reads RAM too.
    void log(PGM_P fmt, ...) {
        char msg[LOG_LINE_SIZE];
        va_list args;
        va_start(args, fmt);
        vsnprintf_P(msg, sizeof(msg), fmt, args);
        va_end(args);

        // Time prefix (always fits) and the message, cut if it is long
        size_t k = (size_t)snprintf_P(_buf[_head], LOG_LINE_SIZE, PSTR("[%lus] "), millis() / 1000);
        strlcpy(_buf[_head] + k, msg, LOG_LINE_SIZE - k);
        Serial.println(_buf[_head]);

        if (++_head >= LOG_BUFFER_SIZE) {
            _head = 0;
            _wrapped = true;
        }
    }

    uint8_t     count()      const { return _wrapped ? LOG_BUFFER_SIZE : _head; }
    uint8_t     startIndex() const { return _wrapped ? _head : 0; }
    const char* entry(uint8_t idx) const { return _buf[idx % LOG_BUFFER_SIZE]; }

private:
    char    _buf[LOG_BUFFER_SIZE][LOG_LINE_SIZE];
    uint8_t _head;
    bool    _wrapped;
};
