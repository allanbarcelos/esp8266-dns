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

    void log(const char* fmt, ...) {
        char msg[LOG_LINE_SIZE];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        snprintf(_buf[_head], LOG_LINE_SIZE, "[%lus] %s", millis() / 1000, msg);
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
