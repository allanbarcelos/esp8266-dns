// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include "IoTCatalog.h"

// Text commands, the same on the panel, the local API, ntfy and MQTT:
//   on | off | toggle | pulse 5[s] | 50% | level 50[%] | brightness 50
//   angle 90 | open | close | color #ff8800 | auto | status
// Case does not matter. "open" and "close" are on and off: the servo goes
// to its open or closed angle.
namespace iot {

enum Op : uint8_t {
    OP_NONE, OP_ON, OP_OFF, OP_TOGGLE, OP_PULSE, OP_LEVEL, OP_ANGLE, OP_COLOR,
    OP_AUTO, OP_STATUS
};

struct Command {
    uint8_t  op    = OP_NONE;
    uint32_t value = 0;   // seconds, %, degrees or 0xRRGGBB
};

// In flash: the RAM is what TLS needs
struct _Word { char word[11]; uint8_t op; };

static const _Word _WORDS[] PROGMEM = {
    {"on", OP_ON}, {"open", OP_ON},
    {"off", OP_OFF}, {"close", OP_OFF},
    {"toggle", OP_TOGGLE},
    {"pulse", OP_PULSE},
    {"level", OP_LEVEL}, {"brightness", OP_LEVEL},
    {"angle", OP_ANGLE},
    {"color", OP_COLOR},
    {"auto", OP_AUTO},
    {"status", OP_STATUS},
};

// Range of each value, for what comes after the word. COLOR is read
// separately (hexadecimal).
inline bool _range(uint8_t op, uint32_t& lo, uint32_t& hi) {
    switch (op) {
        case OP_PULSE: lo = 1; hi = 3600; return true;
        case OP_LEVEL: lo = 0; hi = 100;  return true;
        case OP_ANGLE: lo = 0; hi = 180;  return true;
        default:       return false;
    }
}

// Unsigned decimal with an optional suffix (s or %). Rejects empty input and junk.
inline bool _number(const char* s, char suffix, uint32_t& out) {
    uint32_t v = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (++digits > 5) return false;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    if (digits == 0) return false;
    if (*s == suffix && suffix) s++;
    if (*s) return false;
    out = v;
    return true;
}

inline bool _hexColor(const char* s, uint32_t& out) {
    if (*s == '#') s++;
    uint32_t v = 0;
    int n = 0;
    for (; *s; s++, n++) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else return false;
        v = (v << 4) | d;
    }
    if (n != 6) return false;
    out = v;
    return true;
}

// Text → command. False if the word or the value is not valid.
inline bool parseCommand(const char* text, Command& out) {
    char buf[40];
    size_t n = 0;
    while (*text == ' ') text++;
    for (; *text && n < sizeof(buf) - 1; text++) buf[n++] = (char)tolower((unsigned char)*text);
    if (*text) return false;               // too long
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';
    if (n == 0) return false;

    char* arg = strchr(buf, ' ');

    // "50%" alone is the level
    if (!arg && buf[n - 1] == '%') {
        uint32_t v = 0;
        if (!_number(buf, '%', v) || v > 100) return false;
        out.op = OP_LEVEL;
        out.value = v;
        return true;
    }

    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') arg++;
    }

    uint8_t op = OP_NONE;
    for (const _Word& w : _WORDS) {
        if (strcmp_P(buf, w.word) == 0) {
            op = pgm_read_byte(&w.op);
            break;
        }
    }
    if (op == OP_NONE) return false;

    uint32_t value = 0;
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (op == OP_COLOR) {
        if (!arg || !_hexColor(arg, value)) return false;
    } else if (_range(op, lo, hi)) {
        char suffix = op == OP_PULSE ? 's' : (op == OP_LEVEL ? '%' : '\0');
        if (!arg || !_number(arg, suffix, value) || value < lo || value > hi) return false;
    } else if (arg) {
        return false;                       // there is no "on 5"
    }
    out.op = op;
    out.value = value;
    return true;
}

// Does the command make sense for this capability?
inline bool commandAllowed(uint8_t cap, uint8_t op) {
    if (cap == C_NONE) return false;
    if (op == OP_LEVEL) return cap == C_LEVEL || cap == C_COLOR;
    if (op == OP_ANGLE) return cap == C_POSITION;
    if (op == OP_COLOR) return cap == C_COLOR;
    return true;
}

} // namespace iot
