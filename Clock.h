// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include <time.h>

// Wall clock over SNTP. Needed to check TLS certificate validity (BearSSL
// rejects certificates while the clock is in 1970).
class Clock {
public:
    using TimeFn = time_t (*)();

    explicit Clock(TimeFn fn = _systemTime) : _fn(fn) {}

    // lwIP without extras (less flash) keeps a single SNTP server. With no
    // time after RETRY_MS it switches to the other, alternating until synced.
    void begin() {
        _alt = false;
        _last = 0;
        _configure();
    }

    void tick(unsigned long ms) {
        if (synced() || ms - _last < RETRY_MS) return;
        _last = ms;
        _alt = !_alt;
        _configure();
    }

    time_t now()    const { return _fn(); }
    bool   synced() const { return now() > MIN_VALID_EPOCH; }

    static constexpr time_t MIN_VALID_EPOCH = 1700000000; // Nov 2023
    static constexpr unsigned long RETRY_MS = 60000;

private:
    TimeFn        _fn;
    bool          _alt  = false;
    unsigned long _last = 0;

    void _configure() { configTime(0, 0, _alt ? "time.google.com" : "pool.ntp.org"); }

    static time_t _systemTime() { return time(nullptr); }
};
