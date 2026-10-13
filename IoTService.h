// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include <Servo.h>
#include "Config.h"
#include "Logger.h"
#include "IoTCatalog.h"
#include "IoTCommand.h"
#include "IoTState.h"
#include "Ws2812.h"

// Um sensor e um atuador no ESP-01, cada um opcional:
// - só sensor: lê e publica no ntfy;
// - só atuador: obedece comandos (painel, API local, ntfy);
// - os dois: com a regra ligada, o sensor comanda o atuador no modo
//   automático. Um comando manual vale até "auto".
// A configuração é copiada em begin(): uma gravação que falha não muda a
// saída que já está no ar.
class IoTService {
public:
    static constexpr unsigned long DEBOUNCE_MS      = 200;
    static constexpr unsigned long READ_GAP_MS      = 2000;
    static constexpr unsigned long DS_CONVERT_MS    = 800;
    // Pulso de início em LOW: DHT11 pede ≥ 18 ms, DHT22 ≥ 1 ms (2 pela
    // resolução do millis). Acima do máximo o sensor pode não responder.
    static constexpr unsigned long DHT11_WAKE_MS    = 18;
    static constexpr unsigned long DHT11_WAKE_MAX   = 30;
    static constexpr unsigned long DHT22_WAKE_MS    = 2;
    static constexpr unsigned long DHT22_WAKE_MAX   = 20;
    static constexpr unsigned long PULSE_WINDOW_MS  = 10000;
    static constexpr size_t        MSG_LEN          = 80;
    static constexpr uint8_t       MAX_LEDS         = 60;  // 1,8 ms sem interrupção

    enum Result : uint8_t { OK, INVALID, UNSUPPORTED, NO_ACTUATOR };

    IoTService(Config& cfg, Logger& log) : _cfg(cfg), _log(log) { _clear(); }

    static bool allowedPin(int pin) { return iot::allowedPin(pin); }

    static bool parsePin(const String& s, int8_t& out) {
        if (s == "-1") {
            out = -1;
            return true;
        }
        if (s.length() == 1 && s.c_str()[0] >= '0' && s.c_str()[0] <= '3') {
            out = (int8_t)(s.c_str()[0] - '0');
            return true;
        }
        return false;
    }

    // Inteiro com sinal opcional. Recusa vazio, lixo e fora da faixa.
    static bool parseI16(const String& s, int min, int max, int16_t& out) {
        if (s.length() == 0 || s.length() > 6) return false;
        int i = 0;
        int sign = 1;
        if (s.c_str()[0] == '-') {
            if (s.length() == 1) return false;
            sign = -1;
            i = 1;
        }
        int v = 0;
        for (; i < (int)s.length(); i++) {
            char c = s.c_str()[i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        v *= sign;
        if (v < min || v > max) return false;
        out = (int16_t)v;
        return true;
    }

    static bool parseU8(const String& s, int min, int max, uint8_t& out) {
        int16_t v = 0;
        if (!parseI16(s, min, max, v)) return false;
        out = (uint8_t)v;
        return true;
    }

    void begin() {
        _release();
        _detachSensor();
        _clear();
        _r = _cfg.iot;
        if (_r.dutyOn > 100) _r.dutyOn = 100;
        if (_r.dutyOff > 100) _r.dutyOff = 100;
        if (_r.leds == 0 || _r.leds > MAX_LEDS) _r.leds = MAX_LEDS;

        if (allowedPin(_r.sensorPin)) {
            _sensor = iot::sensorIndex(_r.sensor);
            if (_sensor < 0 && _r.sensor[0]) _unknown("sensor");
        }
        if (allowedPin(_r.actuatorPin) && !(_sensor >= 0 && _r.actuatorPin == _r.sensorPin)) {
            _act = iot::actuatorIndex(_r.actuator);
            if (_act < 0 && _r.actuator[0]) _unknown("atuador");
        }

        if (_sensor >= 0) {
            const iot::SensorDef& s = iot::SENSORS[_sensor];
            _stateName(_onText, _r.onText, s.on);
            _stateName(_offText, _r.offText, s.off);
            pinMode((uint8_t)_r.sensorPin, INPUT_PULLUP);
            if (s.kind == iot::K_PULSE) attachInterrupt((uint8_t)_r.sensorPin, onPulse, FALLING);
            _log.log(PSTR("IoT: sensor %s GPIO%d"), s.slug, _r.sensorPin);
        }
        if (_act >= 0) {
            _level = _r.dutyOn;
            _angle = _r.angleOn;
            if (_r.restore) _restore();
            if (capability() == iot::C_POSITION) {
                _servoOk = _servo.attach((int)_r.actuatorPin) != 0;
                if (!_servoOk) _log.log(PSTR("IoT: servo indisponivel"));
            } else {
                pinMode((uint8_t)_r.actuatorPin, OUTPUT);
            }
            _onSince = millis();
            _drive();
            _log.log(PSTR("IoT: atuador %s GPIO%d"), iot::ACTUATORS[_act].slug, _r.actuatorPin);
        }
    }

    void tick(unsigned long now) {
        if (_sensor >= 0) _tickSensor(now);
        if (_act < 0) return;
        if (_pulsing && (long)(now - _pulseUntil) >= 0) {
            _pulsing = false;
            _setOn(false, now);
            if (hasRule()) {
                _manual = false;
                if (_haveRule) _takeRule();
            }
            _changed(PSTR("[atuador] %s"));
            _persist();
        }
        if (_on && _r.maxOn && now - _onSince >= (unsigned long)_r.maxOn * 60000UL) {
            _pulsing = false;
            _setOn(false, now);
            _manual = hasRule();
            _changed(PSTR("[atuador] %s por seguranca"));
            _persist();
        }
    }

    bool hasSensor()   const { return _sensor >= 0; }
    // Configuração de versão mais nova: a parte fica desligada, o arquivo intacto
    bool unknownType() const { return _unknownType; }
    bool hasActuator() const { return _act >= 0; }
    bool hasRule()     const { return _r.rule && hasSensor() && hasActuator(); }
    bool manual()      const { return _manual; }
    bool actuatorOn()  const { return _on; }
    uint8_t level()    const { return _level; }
    uint8_t angle()    const { return _on ? _angle : _r.angleOff; }
    uint32_t color()   const { return _rgb; }

    uint8_t sensorKind() const { return _sensor >= 0 ? iot::SENSORS[_sensor].kind : (uint8_t)iot::K_NONE; }
    uint8_t capability() const { return _act >= 0 ? iot::ACTUATORS[_act].cap : (uint8_t)iot::C_NONE; }
    const iot::SensorDef*   sensorDef()   const { return _sensor >= 0 ? &iot::SENSORS[_sensor] : nullptr; }
    const iot::ActuatorDef* actuatorDef() const { return _act >= 0 ? &iot::ACTUATORS[_act] : nullptr; }

    // Texto do painel, da API ou do ntfy
    uint8_t command(const char* text, unsigned long now) {
        iot::Command c;
        if (!iot::parseCommand(text, c)) return INVALID;
        return run(c, now);
    }

    uint8_t run(const iot::Command& c, unsigned long now) {
        if (_act < 0) return NO_ACTUATOR;
        if (!iot::commandAllowed(capability(), c.op)) return UNSUPPORTED;
        if (c.op == iot::OP_STATUS) return OK;
        _pulsing = false;
        if (c.op == iot::OP_AUTO) {
            _manual = false;
            if (hasRule() && _haveRule) _takeRule();
            _changed(PSTR("[atuador] %s"));
            _persist();
            return OK;
        }
        switch (c.op) {
            case iot::OP_ON:     _turnOn(now); break;
            case iot::OP_OFF:    _setOn(false, now); break;
            case iot::OP_TOGGLE:
                if (_on) _setOn(false, now);
                else _turnOn(now);
                break;
            case iot::OP_PULSE:
                _turnOn(now);
                _pulsing = true;
                _pulseUntil = now + c.value * 1000UL;
                break;
            case iot::OP_LEVEL:
                _level = (uint8_t)c.value;
                _setOn(c.value > 0, now);
                break;
            case iot::OP_ANGLE:
                _angle = (uint8_t)c.value;
                _setOn(c.value != _r.angleOff, now);
                break;
            default: // OP_COLOR
                _rgb = c.value;
                if (_level == 0) _level = 100;
                _setOn(true, now);
                break;
        }
        _manual = hasRule();
        _changed(PSTR("[atuador] %s"));
        if (!_pulsing) _persist();
        return OK;
    }

    // Leitura do sensor em texto curto
    const char* sensorText() const {
        if (_sensor < 0) return "nenhum";
        if (_failing) return "falha";
        if (!_haveLevel) return "aguardando";
        return _sensorLine;
    }

    const char* actuatorText() const {
        if (_act < 0) return "nenhum";
        if (capability() == iot::C_POSITION && !_servoOk) return "falha";
        return _actLine;
    }

    // Estado em JSON (API e painel). Parte ausente vai como null.
    size_t stateJson(char* out, size_t n) const {
        size_t k = (size_t)snprintf(out, n, "{\"api\":1,\"sensor\":");
        k += _sensorJson(out + k, _left(n, k));
        k += (size_t)snprintf(out + k, _left(n, k), ",\"actuator\":");
        k += _actuatorJson(out + k, _left(n, k));
        k += (size_t)snprintf(out + k, _left(n, k), "}");
        return k;
    }

    bool pending() const { return _pending[0] != '\0'; }
    // Mudança (comando, estado novo) sai em segundos; leitura periódica, por minuto
    bool urgent() const { return _urgent; }
    const char* message() const { return _pending; }
    void ack() {
        _pending[0] = '\0';
        _urgent = false;
    }

    // Contagem de pulsos. O ISR só incrementa.
    static void IRAM_ATTR onPulse() { _pulses++; }

private:
    Config& _cfg;
    Logger& _log;
    IoTConfig _r;
    int8_t  _sensor;
    int8_t  _act;
    bool    _unknownType;

    // Sensor
    bool    _seen;
    bool    _haveLevel;
    bool    _failing;
    bool    _haveRead;
    bool    _windowOpen;
    bool    _active;        // digital: estado ativo
    bool    _edgeReady;
    int     _candidate;
    int     _stable;
    int     _hum;
    int     _temp;
    int     _value;         // distância (cm) ou pulsos
    unsigned long _since;
    unsigned long _lastRead;
    unsigned long _owSince;
    unsigned long _dhtSince;
    unsigned long _windowStart;
    uint8_t _owState;
    bool    _dhtWaking;
    char    _onText[16];
    char    _offText[16];
    char    _sensorLine[32];

    // Atuador
    bool    _on;
    bool    _manual;
    bool    _servoOk;
    bool    _pulsing;
    bool    _haveRule;      // a regra já decidiu ao menos uma vez
    bool    _ruleOn;
    uint8_t _ruleLevel;
    uint8_t _level;
    uint8_t _angle;
    uint32_t _rgb;
    unsigned long _pulseUntil;
    unsigned long _onSince;
    char    _actLine[28];
    char    _pending[MSG_LEN];
    bool    _urgent;
    Servo   _servo;

    static inline volatile uint32_t _pulses = 0;
    static constexpr uint8_t OW_IDLE = 0;
    static constexpr uint8_t OW_WAIT = 1;

    void _clear() {
        _sensor = -1;
        _act = -1;
        _unknownType = false;
        _seen = false;
        _haveLevel = false;
        _failing = false;
        _haveRead = false;
        _windowOpen = false;
        _active = false;
        _edgeReady = false;
        _candidate = HIGH;
        _stable = HIGH;
        _hum = 0;
        _temp = 0;
        _value = 0;
        _since = 0;
        _lastRead = 0;
        _owSince = 0;
        _dhtSince = 0;
        _windowStart = 0;
        _owState = OW_IDLE;
        _dhtWaking = false;
        _onText[0] = '\0';
        _offText[0] = '\0';
        _sensorLine[0] = '\0';
        _on = false;
        _manual = false;
        _servoOk = false;
        _pulsing = false;
        _haveRule = false;
        _ruleOn = false;
        _ruleLevel = 0;
        _level = 100;
        _angle = 90;
        _rgb = 0xFFFFFF;
        _pulseUntil = 0;
        _onSince = 0;
        _actLine[0] = '\0';
        _pending[0] = '\0';
        _urgent = false;
        _pulses = 0;
    }

    void _unknown(const char* part) {
        _unknownType = true;
        _log.log(PSTR("IoT: tipo de %s desconhecido"), part);
    }

    static size_t _left(size_t n, size_t used) { return n > used ? n - used : 0; }

    // Nome do estado digital: o da configuração ou o padrão do tipo
    static void _stateName(char* dst, const char* custom, PGM_P fallback) {
        if (custom[0]) {
            strlcpy(dst, custom, 16);
        } else if (fallback) {
            strncpy_P(dst, fallback, 15);
            dst[15] = '\0';
        }
    }

    void _detachSensor() {
        if (sensorKind() == iot::K_PULSE) detachInterrupt((uint8_t)_r.sensorPin);
    }

    // GPIO0 e GPIO2 têm de ficar altos no próximo boot
    void _idleStrappingPin() {
        if (_r.actuatorPin == 0 || _r.actuatorPin == 2)
            pinMode((uint8_t)_r.actuatorPin, INPUT_PULLUP);
    }

    // Deixa a saída anterior desligada antes de reconfigurar. O liga/desliga
    // segura o nível desligado (pull-up ligaria um módulo que arma em alto).
    void _release() {
        if (_act < 0) return;
        uint8_t cap = capability();
        _on = false;
        if (cap == iot::C_ONOFF) {
            _drive();
            return;
        }
        if (cap == iot::C_POSITION) {
            if (_servoOk) _servo.detach();
            _servoOk = false;
        } else if (cap == iot::C_LEVEL) {
            analogWrite((uint8_t)_r.actuatorPin, 0);
        } else {
            ws2812Show((uint8_t)_r.actuatorPin, _r.leds, 0);
        }
        _idleStrappingPin();
    }

    void _restore() {
        IoTState st;
        if (!st.load()) return;
        _on = st.on;
        _manual = st.manual;
        _level = st.level > 100 ? 100 : st.level;
        _angle = st.angle > 180 ? 180 : st.angle;
        _rgb = st.rgb & 0xFFFFFF;
    }

    void _persist() const {
        if (!_r.restore) return;
        IoTState st;
        st.manual = _manual;
        st.on = _on;
        st.level = _level;
        st.angle = _angle;
        st.rgb = _rgb;
        st.save();
    }

    void _setOn(bool on, unsigned long now) {
        if (on && !_on) _onSince = now;
        _on = on;
    }

    // Ligar volta à posição ou intensidade de "ligado" quando não há outra
    void _turnOn(unsigned long now) {
        uint8_t cap = capability();
        if (cap == iot::C_POSITION) _angle = _r.angleOn;
        else if (_level == 0) _level = cap == iot::C_LEVEL ? _r.dutyOn : 100;
        _setOn(true, now);
    }

    static uint32_t _scale(uint32_t rgb, uint8_t level) {
        uint32_t r = ((rgb >> 16) & 0xFF) * level / 100;
        uint32_t g = ((rgb >> 8) & 0xFF) * level / 100;
        uint32_t b = (rgb & 0xFF) * level / 100;
        return (r << 16) | (g << 8) | b;
    }

    void _drive() {
        uint8_t pin = (uint8_t)_r.actuatorPin;
        switch (capability()) {
            case iot::C_ONOFF:
                digitalWrite(pin, _on == _r.actuatorHigh ? HIGH : LOW);
                break;
            case iot::C_LEVEL:
                analogWrite(pin, (int)(_on ? _level : _r.dutyOff) * 1023 / 100);
                break;
            case iot::C_POSITION:
                if (_servoOk) _servo.write(angle());
                break;
            default: // C_COLOR
                ws2812Show(pin, _r.leds, _on ? _scale(_rgb, _level) : 0);
                break;
        }
        _describe();
    }

    void _describe() {
        uint8_t cap = capability();
        if (cap == iot::C_LEVEL && _on)
            snprintf(_actLine, sizeof(_actLine), "ligado %u%%", _level);
        else if (cap == iot::C_POSITION)
            snprintf(_actLine, sizeof(_actLine), "%s %u graus", _on ? "aberto" : "fechado", angle());
        else if (cap == iot::C_COLOR && _on)
            snprintf(_actLine, sizeof(_actLine), "ligado #%06lx %u%%", (unsigned long)_rgb, _level);
        else
            strlcpy(_actLine, _on ? "ligado" : "desligado", sizeof(_actLine));
    }

    // Saída mudou por comando ou tempo: aplica, registra e avisa
    void _changed(PGM_P fmt) {
        _drive();
        char line[MSG_LEN];
        snprintf_P(line, sizeof(line), fmt, actuatorText());
        _log.log(PSTR("IoT: %s"), line);
        if (_r.notify) {
            strlcpy(_pending, line, sizeof(_pending));
            _urgent = true;
        }
    }

    // Leva o atuador à última decisão da regra (modo automático). Não
    // aciona a saída: quem chama decide (_drive ou _changed).
    bool _takeRule() {
        uint8_t cap = capability();
        bool changed = _ruleOn != _on;
        if (_proportional()) {
            changed = changed || _ruleLevel != _level;
            _level = _ruleLevel;
        } else if (_ruleOn && !_on) {
            if (cap == iot::C_POSITION) _angle = _r.angleOn;
            else if (cap == iot::C_LEVEL) _level = _r.dutyOn;
        }
        _setOn(_ruleOn, millis());
        return changed;
    }

    bool _proportional() const {
        return _r.linear && capability() == iot::C_LEVEL && sensorKind() != iot::K_DIGITAL;
    }

    // ── Sensor ───────────────────────────────────────────────────────────────

    void _tickSensor(unsigned long now) {
        switch (sensorKind()) {
            case iot::K_DHT11:
            case iot::K_DHT22: _tickDht(now); return;
            case iot::K_DS18:  _tickDs(now); return;
            case iot::K_PULSE: _tickPulse(now); return;
            case iot::K_RANGE: _tickRange(now); return;
            default: break;
        }
        int pin = digitalRead((uint8_t)_r.sensorPin);
        if (!_seen || pin != _candidate) {
            _seen = true;
            _candidate = pin;
            _since = now;
            return;
        }
        if (now - _since < DEBOUNCE_MS) return;
        if (_haveLevel && pin == _stable) return;
        _stable = pin;
        _haveLevel = true;
        _onDigital((pin == HIGH) == _r.sensorHigh);
    }

    void _onDigital(bool active) {
        _active = active;
        strlcpy(_sensorLine, active ? _onText : _offText, sizeof(_sensorLine));
        if (hasRule()) {
            if (_r.toggle) {
                if (_edgeReady && active) _ruleOn = !_on;
                _edgeReady = true;
            } else {
                _ruleOn = active;
            }
            _haveRule = true;
            if (!_manual && _takeRule()) _drive();
        }
        _report(true);
    }

    // Leitura vai ao log e, com o ntfy ligado, ao tópico (com o atuador)
    void _report(bool log) {
        char line[MSG_LEN];
        if (_act >= 0)
            snprintf(line, sizeof(line), "[sensor] %s | %s", sensorText(), actuatorText());
        else
            snprintf(line, sizeof(line), "[sensor] %s", sensorText());
        if (log) _log.log(PSTR("IoT: %s"), line + 9);
        if (_r.notify) {
            strlcpy(_pending, line, sizeof(_pending));
            _urgent = _urgent || log;
        }
    }

    void _fail() {
        if (_failing) return;
        _failing = true;
        _log.log(PSTR("IoT: leitura falhou"));
    }

    // Décimos de grau ou de umidade: cabem em int16_t, e o texto, no buffer
    static void _fmtTenths(char* b, size_t n, int16_t tenths) {
        const char* sign = tenths < 0 ? "-" : "";
        int v = tenths < 0 ? -tenths : tenths;
        snprintf(b, n, "%s%d.%d", sign, v / 10, v % 10);
    }

    // value em décimos, exceto distância e pulsos. A margem evita oscilar.
    bool _rule(int value, bool tenths) const {
        int scale = tenths ? 10 : 1;
        int limit = (int)_r.threshold * scale;
        int margin = (int)_r.margin * scale;
        if (_haveRule && _ruleOn) {
            if (_r.above) return value >= limit - margin;
            return value <= limit + margin;
        }
        if (_r.above) return value >= limit;
        return value <= limit;
    }

    // Limite alto acima do limite: interpola. Faixa vazia vira dois patamares.
    uint8_t _mapDuty(int value, bool tenths) const {
        int scale = tenths ? 10 : 1;
        int lo = (int)_r.threshold * scale;
        int hi = (int)_r.thresholdHi * scale;
        if (hi <= lo) return value >= lo ? _r.dutyOn : _r.dutyOff;
        if (value <= lo) return _r.dutyOff;
        if (value >= hi) return _r.dutyOn;
        int delta = (int)_r.dutyOn - (int)_r.dutyOff;
        return (uint8_t)((int)_r.dutyOff + delta * (value - lo) / (hi - lo));
    }

    // Nova leitura numérica: texto, regra e aviso
    void _accept(int value, bool tenths) {
        _failing = false;
        uint8_t kind = sensorKind();
        char a[12];
        if (kind == iot::K_PULSE) {
            snprintf(_sensorLine, sizeof(_sensorLine), "%d pulsos/10 s", value);
        } else if (kind == iot::K_RANGE) {
            snprintf(_sensorLine, sizeof(_sensorLine), "%d cm", value);
        } else if (kind == iot::K_DS18) {
            _fmtTenths(a, sizeof(a), value);
            snprintf(_sensorLine, sizeof(_sensorLine), "%s C", a);
        } else {
            char b[12];
            _fmtTenths(a, sizeof(a), _temp);
            _fmtTenths(b, sizeof(b), _hum);
            snprintf(_sensorLine, sizeof(_sensorLine), "%s C, %s%%", a, b);
        }
        bool log = !_haveLevel;
        _haveLevel = true;
        if (hasRule()) {
            if (_proportional()) {
                _ruleLevel = _mapDuty(value, tenths);
                _ruleOn = _ruleLevel != _r.dutyOff;
            } else {
                _ruleOn = _rule(value, tenths);
            }
            _haveRule = true;
            if (!_manual && _takeRule()) {
                _drive();
                log = true;
            }
        }
        _report(log);
    }

    bool _waitLevel(int level, unsigned long timeoutUs) {
        unsigned long start = micros();
        while (digitalRead((uint8_t)_r.sensorPin) != level) {
            if ((unsigned long)(micros() - start) > timeoutUs) return false;
        }
        return true;
    }

    // O pulso do DHT dura dezenas de microssegundos. A ISR do Wi-Fi
    // estica esse intervalo, então a leitura fica com a interrupção fechada.
    bool _dhtBits(uint8_t data[5]) {
        for (int i = 0; i < 5; i++) data[i] = 0;
        if (!_waitLevel(LOW, 300)) return false;
        if (!_waitLevel(HIGH, 300)) return false;
        if (!_waitLevel(LOW, 300)) return false;
        for (int i = 0; i < 40; i++) {
            if (!_waitLevel(HIGH, 200)) return false;
            unsigned long mark = micros();
            if (!_waitLevel(LOW, 200)) return false;
            uint8_t bit = (unsigned long)(micros() - mark) > 40 ? 1 : 0;
            data[i / 8] = (uint8_t)((data[i / 8] << 1) | bit);
        }
        return true;
    }

    // Fase 1: pino em LOW; o tick seguinte que cair na janela lê os bits.
    void _dhtWake(unsigned long now) {
        uint8_t pin = (uint8_t)_r.sensorPin;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        _dhtWaking = true;
        _dhtSince = now;
    }

    // Fase 2: solta o pino e lê os 40 bits (µs, com a interrupção fechada)
    bool _readDht(uint8_t data[5]) {
        uint8_t pin = (uint8_t)_r.sensorPin;
        digitalWrite(pin, HIGH);
        delayMicroseconds(40);
        pinMode(pin, INPUT_PULLUP);

        noInterrupts();
        bool ok = _dhtBits(data);
        interrupts();
        if (!ok) return false;
        uint8_t sum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
        return sum == data[4];
    }

    void _decodeDht(const uint8_t d[5]) {
        if (sensorKind() == iot::K_DHT11) {
            _hum = d[0] * 10 + d[1];
            _temp = d[2] * 10 + d[3];
            return;
        }
        _hum = (d[0] << 8) | d[1];
        int t = ((d[2] & 0x7F) << 8) | d[3];
        if (d[2] & 0x80) t = -t;
        _temp = t;
    }

    // Sem delay: o pulso de início atravessa dois ticks e o loop segue
    void _tickDht(unsigned long now) {
        if (_dhtWaking) {
            bool dht11 = sensorKind() == iot::K_DHT11;
            unsigned long held = now - _dhtSince;
            if (held < (dht11 ? DHT11_WAKE_MS : DHT22_WAKE_MS)) return;
            if (held > (dht11 ? DHT11_WAKE_MAX : DHT22_WAKE_MAX)) {
                _dhtWake(now); // o loop atrasou: recomeça o pulso
                return;
            }
            _dhtWaking = false;
            uint8_t data[5];
            if (!_readDht(data)) {
                _fail();
                return;
            }
            _decodeDht(data);
            _accept(_r.metric == 1 ? _temp : _hum, true);
            return;
        }
        if (_haveRead && now - _lastRead < READ_GAP_MS) return;
        _lastRead = now;
        _haveRead = true;
        _dhtWake(now);
    }

    bool _owReset() {
        uint8_t pin = (uint8_t)_r.sensorPin;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        delayMicroseconds(480);
        pinMode(pin, INPUT_PULLUP);
        delayMicroseconds(70);
        bool presence = digitalRead(pin) == LOW;
        delayMicroseconds(410);
        return presence;
    }

    void _owWriteByte(uint8_t v) {
        uint8_t pin = (uint8_t)_r.sensorPin;
        for (int i = 0; i < 8; i++) {
            int bit = v & 1;
            pinMode(pin, OUTPUT);
            digitalWrite(pin, LOW);
            delayMicroseconds(bit ? 6 : 60);
            pinMode(pin, INPUT_PULLUP);
            delayMicroseconds(bit ? 64 : 10);
            v = (uint8_t)(v >> 1);
        }
    }

    uint8_t _owReadByte() {
        uint8_t pin = (uint8_t)_r.sensorPin;
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) {
            pinMode(pin, OUTPUT);
            digitalWrite(pin, LOW);
            delayMicroseconds(3);
            pinMode(pin, INPUT_PULLUP);
            delayMicroseconds(8);
            if (digitalRead(pin) == HIGH) v = (uint8_t)(v | (1 << i));
            delayMicroseconds(50);
        }
        return v;
    }

    static uint8_t _crc8(const uint8_t* d, int n) {
        uint8_t crc = 0;
        for (int i = 0; i < n; i++) {
            uint8_t in = d[i];
            for (int b = 0; b < 8; b++) {
                uint8_t mix = (uint8_t)((crc ^ in) & 1);
                crc = (uint8_t)(crc >> 1);
                if (mix) crc = (uint8_t)(crc ^ 0x8C);
                in = (uint8_t)(in >> 1);
            }
        }
        return crc;
    }

    bool _owRead(int* tenths) {
        if (!_owReset()) return false;
        _owWriteByte(0xCC);
        _owWriteByte(0xBE);
        uint8_t b[9];
        for (int i = 0; i < 9; i++) b[i] = _owReadByte();
        if (_crc8(b, 8) != b[8]) return false;
        int16_t raw = (int16_t)(b[0] | (b[1] << 8));
        *tenths = (int)(((int32_t)raw * 10) / 16);
        return true;
    }

    void _tickDs(unsigned long now) {
        if (_owState == OW_WAIT) {
            if (now - _owSince < DS_CONVERT_MS) return;
            _owState = OW_IDLE;
            int tenths = 0;
            if (!_owRead(&tenths)) {
                _fail();
                return;
            }
            _temp = tenths;
            _accept(tenths, true);
            return;
        }
        if (_haveRead && now - _lastRead < READ_GAP_MS) return;
        _lastRead = now;
        _haveRead = true;
        if (!_owReset()) {
            _fail();
            return;
        }
        _owWriteByte(0xCC);
        _owWriteByte(0x44);
        _owState = OW_WAIT;
        _owSince = now;
    }

    // HC-SR04 com trig e echo no mesmo pino. A distância sai em cm.
    bool _readRange(int* cm) {
        uint8_t pin = (uint8_t)_r.sensorPin;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        delayMicroseconds(2);
        digitalWrite(pin, HIGH);
        delayMicroseconds(10);
        digitalWrite(pin, LOW);
        pinMode(pin, INPUT);
        if (!_waitLevel(HIGH, 30000)) return false;
        unsigned long start = micros();
        if (!_waitLevel(LOW, 30000)) return false;
        int v = (int)((unsigned long)(micros() - start) / 58);
        if (v < 1 || v > 400) return false;
        *cm = v;
        return true;
    }

    void _tickRange(unsigned long now) {
        if (_haveRead && now - _lastRead < READ_GAP_MS) return;
        _lastRead = now;
        _haveRead = true;
        int cm = 0;
        if (!_readRange(&cm)) {
            _fail();
            return;
        }
        _value = cm;
        _accept(cm, false);
    }

    void _tickPulse(unsigned long now) {
        if (!_windowOpen) {
            _windowOpen = true;
            _windowStart = now;
            _pulses = 0;
            return;
        }
        if (now - _windowStart < PULSE_WINDOW_MS) return;
        noInterrupts();
        uint32_t n = _pulses;
        _pulses = 0;
        interrupts();
        _windowStart = now;
        _value = (int)n;
        _accept((int)n, false);
    }

    // ── JSON ─────────────────────────────────────────────────────────────────
    // snprintf direto: o ArduinoJson pediria um documento só para isto.

    size_t _sensorJson(char* out, size_t n) const {
        if (_sensor < 0) return (size_t)snprintf(out, n, "null");
        const char* status = _failing ? "error" : (_haveLevel ? "ok" : "waiting");
        size_t k = (size_t)snprintf(out, n, "{\"type\":\"%s\",\"status\":\"%s\",\"text\":\"%s\"",
                                    iot::SENSORS[_sensor].slug, status, sensorText());
        if (!_failing && _haveLevel) {
            char a[12];
            char b[12];
            switch (sensorKind()) {
                case iot::K_DIGITAL:
                    k += (size_t)snprintf(out + k, _left(n, k), ",\"active\":%s",
                                          _active ? "true" : "false");
                    break;
                case iot::K_DHT11:
                case iot::K_DHT22:
                    _fmtTenths(a, sizeof(a), _temp);
                    _fmtTenths(b, sizeof(b), _hum);
                    k += (size_t)snprintf(out + k, _left(n, k),
                                          ",\"temperature\":%s,\"humidity\":%s", a, b);
                    break;
                case iot::K_DS18:
                    _fmtTenths(a, sizeof(a), _temp);
                    k += (size_t)snprintf(out + k, _left(n, k), ",\"temperature\":%s", a);
                    break;
                case iot::K_RANGE:
                    k += (size_t)snprintf(out + k, _left(n, k), ",\"distance\":%d", _value);
                    break;
                default: // K_PULSE
                    k += (size_t)snprintf(out + k, _left(n, k), ",\"pulses\":%d", _value);
                    break;
            }
        }
        return k + (size_t)snprintf(out + k, _left(n, k), "}");
    }

    size_t _actuatorJson(char* out, size_t n) const {
        if (_act < 0) return (size_t)snprintf(out, n, "null");
        uint8_t cap = capability();
        size_t k = (size_t)snprintf(out, n,
                                    "{\"type\":\"%s\",\"capability\":\"%s\",\"on\":%s,"
                                    "\"mode\":\"%s\",\"text\":\"%s\"",
                                    iot::ACTUATORS[_act].slug, iot::CAP_NAMES[cap],
                                    _on ? "true" : "false",
                                    hasRule() && !_manual ? "auto" : "manual", actuatorText());
        if (cap == iot::C_LEVEL || cap == iot::C_COLOR)
            k += (size_t)snprintf(out + k, _left(n, k), ",\"level\":%u", _level);
        if (cap == iot::C_POSITION)
            k += (size_t)snprintf(out + k, _left(n, k), ",\"angle\":%u,\"ok\":%s",
                                  angle(), _servoOk ? "true" : "false");
        if (cap == iot::C_COLOR)
            k += (size_t)snprintf(out + k, _left(n, k), ",\"color\":\"#%06lx\"",
                                  (unsigned long)_rgb);
        if (_pulsing) {
            long left = (long)(_pulseUntil - millis());
            if (left < 0) left = 0;
            k += (size_t)snprintf(out + k, _left(n, k), ",\"pulse\":%ld", (left + 999) / 1000);
        }
        return k + (size_t)snprintf(out + k, _left(n, k), "}");
    }
};
