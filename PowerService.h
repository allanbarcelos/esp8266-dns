// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include "Config.h"
#include "Logger.h"
#include "I2cBus.h"
#include "IoTCatalog.h"

// Where the power comes from, through the MAX17048 battery gauge (I2C, 0x36).
//
// The gauge gives the charge (%), the cell voltage and the charge rate
// (%/h, signed). A positive rate is the cell filling up: with a battery and
// a panel, that is solar charging. A negative rate, or a charge below full,
// is the device on battery. Full and idle is the supply holding it. With no
// gauge configured, the device runs on the supply. The panel shows the
// source at the top.
//
// The gauge uses the chosen pin (SDA) and its pair (SCL). It shares the bus
// only with an SHT3x on the same pair.
class PowerService {
public:
    enum Source : uint8_t { MAINS, BATTERY, CHARGING, UNKNOWN };

    static constexpr uint8_t       ADDR        = 0x36;
    static constexpr unsigned long READ_MS     = 10000;
    static constexpr int16_t       CHARGE_RATE = 3;    // 3 × 0.208 %/h: charging or draining
    static constexpr uint8_t       FULL        = 98;   // % from which it counts as "full"

    PowerService(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {}

    bool configured() const { return _cfg.bat_sda >= 0; }

    void begin() {
        _read = false;
        _ok = false;
        _logged = NONE;
        if (configured()) _bus().begin();
    }

    void tick(unsigned long now) {
        if (!configured() || (_read && now - _last < READ_MS)) return;
        _last = now;
        _read = true;
        I2cBus bus = _bus();
        uint16_t soc = 0, rate = 0, cell = 0;
        bool ok = bus.readReg16(ADDR, 0x04, soc) && bus.readReg16(ADDR, 0x16, rate) &&
                  bus.readReg16(ADDR, 0x02, cell);
        if (ok) {
            _soc = soc >> 8 > 100 ? 100 : (uint8_t)(soc >> 8);
            _rate = (int16_t)rate;
            _mv = (uint16_t)((uint32_t)cell * 5 / 64);   // 78.125 µV per unit
        }
        _ok = ok;
        Source s = source();
        if (s == _logged) return;
        _logged = s;
        _log.log(PSTR("Power: %s"), name(s));
    }

    // State as text (log and Status)
    static const char* name(Source s) {
        static const char* const NAMES[] = {"on supply", "on battery", "charging", "gauge not answering"};
        return NAMES[s];
    }

    Source source() const {
        if (!configured()) return MAINS;
        if (!_ok) return UNKNOWN;
        if (_rate >= CHARGE_RATE) return CHARGING;
        if (_rate <= -CHARGE_RATE || _soc < FULL) return BATTERY;
        return MAINS;
    }

    uint8_t  percent()    const { return _soc; }
    uint16_t millivolts() const { return _mv; }
    // Rate in tenths of %/h (0.208 %/h per unit)
    int16_t  rateTenths() const { return (int16_t)((int32_t)_rate * 208 / 100); }

    // Pins free for the gauge with this IoT setup: nothing on SDA or SCL,
    // except an SHT3x on the same pair (it is the same bus)
    static bool pinsFree(int8_t sda, const IoTConfig& t) {
        if (sda < 0) return true;
        int8_t scl = I2cBus::sclFor(sda);
        if (t.actuatorPin == sda || t.actuatorPin == scl) return false;
        if (t.sensorPin != sda && t.sensorPin != scl) return true;
        int8_t i = iot::sensorIndex(t.sensor);   // an I2C sensor shares the gauge's pair
        return t.sensorPin == sda && i >= 0 && iot::i2c(iot::SENSORS[i].kind);
    }

private:
    Config&       _cfg;
    Logger&       _log;
    bool          _read = false;
    bool          _ok = false;
    static constexpr uint8_t NONE = 0xFF;
    uint8_t       _logged = NONE;    // state already logged
    unsigned long _last = 0;
    uint8_t       _soc = 0;
    int16_t       _rate = 0;
    uint16_t      _mv = 0;

    I2cBus _bus() const { return I2cBus((uint8_t)_cfg.bat_sda, (uint8_t)I2cBus::sclFor(_cfg.bat_sda)); }
};
