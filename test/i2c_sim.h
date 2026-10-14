// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <functional>
#include <map>
#include <set>
#include <vector>
#include "Arduino.h"

// I2C slaves simulated at the pin level: they decode start, stop,
// bits and ACK from the changes the code makes to the pins (pinMode and
// digitalWrite) and hold SDA low for ACK and data. Each address that is
// present answers a read with reply(address, bytes written before).
struct I2cSim {
    using Reply = std::function<std::vector<uint8_t>(uint8_t addr, const std::vector<uint8_t>& written)>;

    uint8_t sda, scl;
    std::set<uint8_t> present;
    Reply reply = [](uint8_t, const std::vector<uint8_t>&) { return std::vector<uint8_t>(); };

    // What went over the bus
    std::vector<std::vector<uint8_t>> writes;   // per address: [addr, bytes...]
    int starts = 0, stops = 0;
    int nackAfter = -1;                          // NACK on the Nth data byte (test)
    bool refuseRead = false;                     // NACK on the read address

    I2cSim(uint8_t sda, uint8_t scl) : sda(sda), scl(scl) {
        active() = this;
        mock::pinHook = &I2cSim::onPin;
        mock::readHook = &I2cSim::onRead;
        _lastSda = _master(sda);
        _lastScl = _master(scl);
    }
    ~I2cSim() {
        if (active() == this) {
            active() = nullptr;
            mock::pinHook = nullptr;
            mock::readHook = nullptr;
        }
    }

    static I2cSim*& active() { static I2cSim* p = nullptr; return p; }

private:
    enum { IDLE, RX, TX };
    int     _st = IDLE;
    bool    _lastSda, _lastScl, _drive = false, _isAddr = false, _rw = false;
    bool    _acking = false, _ack = false, _masterAck = false;
    int     _bit = 0, _byte = 0, _data = 0;
    uint8_t _shift = 0, _addr = 0;
    std::vector<uint8_t> _tx;

    // Level the master puts on the line: low only as an output at LOW
    static bool _master(uint8_t pin) {
        return !(mock::pinModeV[pin] == OUTPUT && mock::pinOut[pin] == LOW);
    }

    bool _sdaBus() const { return _master(sda) && !_drive; }

    static void onPin() { active()->_edge(); }
    static int onRead(uint8_t pin) {
        I2cSim* s = active();
        if (pin == s->sda) return s->_sdaBus() ? HIGH : LOW;
        if (pin == s->scl) return _master(pin) ? HIGH : LOW;
        return -1;
    }

    void _present(uint8_t b) { _drive = !(b >> (7 - _bit) & 1); }

    void _edge() {
        bool msda = _master(sda), mscl = _master(scl);
        if (mscl && _lastScl && msda != _lastSda) {         // SDA changes while SCL is high
            if (!msda) {                                     // start
                starts++;
                _st = RX;
                _isAddr = true;
                _bit = 0;
                _shift = 0;
                _acking = false;
                _drive = false;
            } else {                                         // stop
                stops++;
                _st = IDLE;
                _drive = false;
            }
        } else if (mscl && !_lastScl) {
            _rise();
        } else if (!mscl && _lastScl) {
            _fall();
        }
        _lastSda = msda;
        _lastScl = mscl;
    }

    void _rise() {
        if (_st == RX && _bit < 8) {
            _shift = (uint8_t)(_shift << 1 | (_sdaBus() ? 1 : 0));
            _bit++;
        } else if (_st == TX && _bit == 8) {
            _masterAck = !_sdaBus();
        }
    }

    void _fall() {
        if (_st == RX) {
            if (_bit == 8 && !_acking) {                     // end of the byte: ACK?
                if (_isAddr) {
                    _addr = (uint8_t)(_shift >> 1);
                    _rw = _shift & 1;
                    _ack = present.count(_addr) > 0 && !(_rw && refuseRead);
                    if (_ack && !_rw) writes.push_back({_addr});
                    if (_ack && _rw) {
                        static const std::vector<uint8_t> none;
                        _tx = reply(_addr, writes.empty() ? none : writes.back());
                    }
                    _data = 0;
                } else {
                    _ack = _data++ != nackAfter;
                    if (_ack) writes.back().push_back(_shift);
                }
                _drive = _ack;
                _acking = true;
            } else if (_acking) {                            // end of the ACK clock
                _acking = false;
                _drive = false;
                if (!_ack) {
                    _st = IDLE;
                } else if (_isAddr && _rw) {
                    _st = TX;
                    _byte = 0;
                    _bit = 0;
                    _present(_next());
                } else {
                    _isAddr = false;
                    _bit = 0;
                    _shift = 0;
                }
            }
        } else if (_st == TX) {
            if (_bit < 8) {
                _bit++;
                if (_bit < 8) _present(_next());
                else _drive = false;                         // the master answers
            } else if (_masterAck) {
                _byte++;
                _bit = 0;
                _present(_next());
            } else {
                _st = IDLE;
                _drive = false;
            }
        }
    }

    uint8_t _next() const { return (size_t)_byte < _tx.size() ? _tx[_byte] : 0xFF; }
};
