// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// Software I2C, master only, around 100 kHz. No Wire library: two small
// sensors (SHT3x, MAX17048) do not pay for the generic driver. A high line
// is the pin released with a pull-up (INPUT_PULLUP); low is an output at
// LOW (open drain). The module needs its own 4.7 kOhm pull-up: the internal
// one is too weak for the edges. No clock stretching: the commands used do
// not need it.
//
// On the ESP-01 the pairs are fixed: GPIO0 with GPIO2, RX (3) with TX (1).
class I2cBus {
public:
    // SCL paired with an SDA pin
    static int8_t sclFor(int8_t sda) { return (int8_t)(sda ^ 2); }

    I2cBus(uint8_t sda, uint8_t scl) : _sda(sda), _scl(scl) {}

    void begin() {
        _high(_sda);
        _high(_scl);
    }

    // n bytes to addr; true if the device and every byte got an ACK.
    // Without a stop, the next read does the repeated start.
    bool write(uint8_t addr, const uint8_t* d, size_t n, bool stop = true) {
        _start();
        bool ok = _put((uint8_t)(addr << 1));
        for (size_t i = 0; ok && i < n; i++) ok = _put(d[i]);
        if (stop || !ok) _stop();
        return ok;
    }

    bool read(uint8_t addr, uint8_t* d, size_t n) {
        _start();
        bool ok = _put((uint8_t)(addr << 1 | 1));
        for (size_t i = 0; ok && i < n; i++) d[i] = _get(i + 1 < n);
        _stop();
        return ok;
    }

    // 16-bit register, most significant byte first (MAX17048)
    bool readReg16(uint8_t addr, uint8_t reg, uint16_t& v) {
        uint8_t b[2];
        if (!write(addr, &reg, 1, false) || !read(addr, b, 2)) return false;
        v = (uint16_t)(b[0] << 8 | b[1]);
        return true;
    }

private:
    uint8_t _sda, _scl;

    static void _wait() { delayMicroseconds(5); }
    static void _high(uint8_t pin) { pinMode(pin, INPUT_PULLUP); }
    static void _low(uint8_t pin) {
        digitalWrite(pin, LOW);
        pinMode(pin, OUTPUT);
    }

    void _start() {
        _high(_sda);
        _high(_scl);
        _wait();
        _low(_sda);
        _wait();
        _low(_scl);
    }

    void _stop() {
        _low(_sda);
        _wait();
        _high(_scl);
        _wait();
        _high(_sda);
        _wait();
    }

    // One clock pulse; returns SDA read while SCL is high
    bool _clock() {
        _wait();
        _high(_scl);
        _wait();
        bool v = digitalRead(_sda) == HIGH;
        _low(_scl);
        return v;
    }

    bool _put(uint8_t b) {
        for (int i = 7; i >= 0; i--) {
            if (b >> i & 1) _high(_sda);
            else _low(_sda);
            _clock();
        }
        _high(_sda);
        return !_clock();       // ACK: the device holds SDA low
    }

    uint8_t _get(bool ack) {
        uint8_t v = 0;
        _high(_sda);
        for (int i = 0; i < 8; i++) v = (uint8_t)(v << 1 | _clock());
        if (ack) _low(_sda);
        _clock();
        _high(_sda);
        return v;
    }
};
