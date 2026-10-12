// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include <time.h>

// Relógio de parede via SNTP. Necessário para validar a validade dos
// certificados TLS (BearSSL rejeita certificados se o relógio estiver em 1970).
class Clock {
public:
    using TimeFn = time_t (*)();

    explicit Clock(TimeFn fn = _systemTime) : _fn(fn) {}

    void begin() {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
    }

    time_t now()    const { return _fn(); }
    bool   synced() const { return now() > MIN_VALID_EPOCH; }

    static constexpr time_t MIN_VALID_EPOCH = 1700000000; // nov/2023

private:
    TimeFn _fn;

    static time_t _systemTime() { return time(nullptr); }
};
