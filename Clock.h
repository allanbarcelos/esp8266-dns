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

    // O lwIP sem extras (menos flash) guarda um servidor SNTP só. Sem hora
    // depois de RETRY_MS, troca para o outro, e assim alterna até sincronizar.
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

    static constexpr time_t MIN_VALID_EPOCH = 1700000000; // nov/2023
    static constexpr unsigned long RETRY_MS = 60000;

private:
    TimeFn        _fn;
    bool          _alt  = false;
    unsigned long _last = 0;

    void _configure() { configTime(0, 0, _alt ? "time.google.com" : "pool.ntp.org"); }

    static time_t _systemTime() { return time(nullptr); }
};
