// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include "Config.h"
#include "Logger.h"
#include "I2cBus.h"

// De onde vem a energia, pelo medidor de bateria MAX17048 (I2C, 0x36).
//
// O medidor dá a carga (%), a tensão da célula e a taxa de carga (%/h,
// com sinal). Taxa positiva é a célula enchendo: com bateria e painel, é o
// solar carregando. Taxa negativa, ou carga abaixo de cheia, é o aparelho
// na bateria. Cheia e parada é a fonte segurando. Sem medidor configurado,
// o aparelho está na fonte. O painel mostra a fonte no topo.
//
// O medidor usa o pino escolhido (SDA) e o do par (SCL). Divide o
// barramento só com o SHT3x no mesmo par.
class PowerService {
public:
    enum Source : uint8_t { MAINS, BATTERY, CHARGING, UNKNOWN };

    static constexpr uint8_t       ADDR        = 0x36;
    static constexpr unsigned long READ_MS     = 10000;
    static constexpr int16_t       CHARGE_RATE = 3;    // 3 × 0,208 %/h: carregando ou gastando
    static constexpr uint8_t       FULL        = 98;   // % a partir da qual "cheia"

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
            _mv = (uint16_t)((uint32_t)cell * 5 / 64);   // 78,125 µV por unidade
        }
        _ok = ok;
        Source s = source();
        if (s == _logged) return;
        _logged = s;
        _log.log(PSTR("Energia: %s"), name(s));
    }

    // Situação em texto (log e Status)
    static const char* name(Source s) {
        static const char* const NAMES[] = {"na fonte", "na bateria", "carregando", "medidor sem resposta"};
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
    // Taxa em décimos de %/h (0,208 %/h por unidade)
    int16_t  rateTenths() const { return (int16_t)((int32_t)_rate * 208 / 100); }

    // Pinos livres para o medidor com este IoT: nada no SDA nem no SCL,
    // a não ser o SHT3x no mesmo par (o barramento é o mesmo)
    static bool pinsFree(int8_t sda, const IoTConfig& t) {
        if (sda < 0) return true;
        int8_t scl = I2cBus::sclFor(sda);
        if (t.actuatorPin == sda || t.actuatorPin == scl) return false;
        if (t.sensorPin != sda && t.sensorPin != scl) return true;
        return t.sensorPin == sda && strcmp(t.sensor, "sht3x") == 0;
    }

private:
    Config&       _cfg;
    Logger&       _log;
    bool          _read = false;
    bool          _ok = false;
    static constexpr uint8_t NONE = 0xFF;
    uint8_t       _logged = NONE;    // situação que já foi ao log
    unsigned long _last = 0;
    uint8_t       _soc = 0;
    int16_t       _rate = 0;
    uint16_t      _mv = 0;

    I2cBus _bus() const { return I2cBus((uint8_t)_cfg.bat_sda, (uint8_t)I2cBus::sclFor(_cfg.bat_sda)); }
};
