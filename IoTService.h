#pragma once
#include <Arduino.h>
#include <Servo.h>
#include "Config.h"
#include "Logger.h"

// Um sensor e um módulo na ESP-01. O tipo vem da web.
// Sem as chaves novas, o sensor é digital e o módulo é nível fixo.
// A leitura numérica comanda o módulo na hora. O ntfy leva uma linha
// só, com o prefixo [sensor], no tópico já cadastrado.
// Os pinos efetivos são copiados em begin(). Uma gravação que falha
// não muda a saída que já está no ar.
class IoTService {
public:
    static constexpr unsigned long DEBOUNCE_MS      = 200;
    static constexpr unsigned long READ_GAP_MS      = 2000;
    static constexpr unsigned long DS_CONVERT_MS    = 800;
    static constexpr unsigned long PULSE_WINDOW_MS  = 10000;
    static constexpr size_t        MSG_LEN          = 64;

    static constexpr uint8_t SENSOR_DIGITAL = 0;
    static constexpr uint8_t SENSOR_DHT11   = 1;
    static constexpr uint8_t SENSOR_DHT22   = 2;
    static constexpr uint8_t SENSOR_DS18B20 = 3;
    static constexpr uint8_t SENSOR_PULSE   = 4;
    static constexpr uint8_t SENSOR_PIR     = 5;
    static constexpr uint8_t SENSOR_DOOR    = 6;
    static constexpr uint8_t SENSOR_RAIN    = 7;
    static constexpr uint8_t SENSOR_FLAME   = 8;
    static constexpr uint8_t SENSOR_GAS     = 9;
    static constexpr uint8_t SENSOR_SOIL    = 10;
    static constexpr uint8_t SENSOR_LIGHT   = 11;
    static constexpr uint8_t SENSOR_SOUND   = 12;
    static constexpr uint8_t SENSOR_FLOAT   = 13;
    static constexpr uint8_t SENSOR_BUTTON  = 14;
    static constexpr uint8_t SENSOR_RANGE   = 15;
    static constexpr uint8_t SENSOR_COUNT   = 16;

    static constexpr uint8_t MODULE_LEVEL  = 0;
    static constexpr uint8_t MODULE_SERVO  = 1;
    static constexpr uint8_t MODULE_LED    = 2;
    static constexpr uint8_t MODULE_BUZZER = 3;
    static constexpr uint8_t MODULE_PUMP   = 4;
    static constexpr uint8_t MODULE_VALVE  = 5;
    static constexpr uint8_t MODULE_FAN    = 6;
    static constexpr uint8_t MODULE_MOTOR  = 7;
    static constexpr uint8_t MODULE_COUNT  = 8;

    // O script da página usa estes números. 0 no sensor é o nível digital.
    static constexpr uint8_t KIND_DIGITAL = 0;
    static constexpr uint8_t KIND_DHT11   = 1;
    static constexpr uint8_t KIND_DHT22   = 2;
    static constexpr uint8_t KIND_DS18    = 3;
    static constexpr uint8_t KIND_PULSE   = 4;
    static constexpr uint8_t KIND_RANGE   = 5;

    static constexpr uint8_t MKIND_LEVEL = 0;
    static constexpr uint8_t MKIND_SERVO = 1;
    static constexpr uint8_t MKIND_PWM   = 2;

    static constexpr uint8_t MANUAL_AUTO = 0;
    static constexpr uint8_t MANUAL_ON   = 1;
    static constexpr uint8_t MANUAL_OFF  = 2;

    static uint8_t sensorKind(uint8_t type) { return _sensorAt(type).kind; }
    static const char* sensorLabel(uint8_t type) { return _sensorAt(type).label; }
    static uint8_t moduleKind(uint8_t type) { return _moduleAt(type).kind; }
    static const char* moduleLabel(uint8_t type) { return _moduleAt(type).label; }

    IoTService(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {
        _runSensor = -1;
        _runModule = -1;
        _runSensorHigh = true;
        _runModuleHigh = false;
        _runNotify = false;
        _runSensorType = SENSOR_DIGITAL;
        _runModuleType = MODULE_LEVEL;
        _runMetric = 0;
        _runAbove = false;
        _runThreshold = 0;
        _runMargin = 0;
        _runAngleOn = 90;
        _runAngleOff = 0;
        _runDutyOn = 100;
        _runDutyOff = 0;
        _runManual = MANUAL_AUTO;
        _runToggle = false;
        _runLinear = false;
        _runThresholdHi = 0;
        _clear();
    }

    static bool allowedPin(int pin) {
        return pin == 0 || pin == 1 || pin == 2 || pin == 3;
    }

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
        _releaseOutput();
        _detachSensor();
        _clear();
        _runSensor     = _cfg.iot_sensor_pin;
        _runModule     = _cfg.iot_module_pin;
        _runSensorHigh = _cfg.iot_sensor_high;
        _runModuleHigh = _cfg.iot_module_high;
        _runNotify     = _cfg.iot_notify;
        _runSensorType = _cfg.iot_sensor_type;
        _runModuleType = _cfg.iot_module_type;
        _runMetric     = _cfg.iot_metric;
        _runAbove      = _cfg.iot_above;
        _runThreshold  = _cfg.iot_threshold;
        _runMargin     = _cfg.iot_margin;
        _runAngleOn    = _cfg.iot_angle_on;
        _runAngleOff   = _cfg.iot_angle_off;
        _runDutyOn     = _clampDuty(_cfg.iot_duty_on);
        _runDutyOff    = _clampDuty(_cfg.iot_duty_off);
        _runManual     = _cfg.iot_manual;
        _runToggle     = _cfg.iot_toggle;
        _runLinear     = _cfg.iot_linear;
        _runThresholdHi = _cfg.iot_threshold_hi;
        if (_runSensorType >= SENSOR_COUNT) _runSensorType = SENSOR_DIGITAL;
        if (_runModuleType >= MODULE_COUNT) _runModuleType = MODULE_LEVEL;
        if (_runManual > MANUAL_OFF) _runManual = MANUAL_AUTO;
        if (!allowedPin(_runSensor)) return;
        pinMode((uint8_t)_runSensor, INPUT_PULLUP);
        if (sensorKind(_runSensorType) == KIND_PULSE) {
            _pulses = 0;
            attachInterrupt((uint8_t)_runSensor, onPulse, FALLING);
        }
        if (_moduleIsOutput()) {
            bool driveOn = _runManual == MANUAL_ON;
            if (moduleKind(_runModuleType) == MKIND_SERVO) {
                _servoOk = _servo.attach((int)_runModule) != 0;
                if (_servoOk) _writeModule(driveOn);
                else _log.log("IoT: servo indisponivel");
                _log.log("IoT: sensor GPIO%d servo GPIO%d", _runSensor, _runModule);
            } else {
                pinMode((uint8_t)_runModule, OUTPUT);
                _writeModule(driveOn);
                _log.log("IoT: sensor GPIO%d modulo GPIO%d", _runSensor, _runModule);
            }
        } else {
            _log.log("IoT: sensor GPIO%d", _runSensor);
        }
    }

    void tick(unsigned long now) {
        if (!allowedPin(_runSensor)) return;
        uint8_t kind = sensorKind(_runSensorType);
        if (kind == KIND_DHT11 || kind == KIND_DHT22) {
            _tickDht(now);
            return;
        }
        if (kind == KIND_DS18) {
            _tickDs(now);
            return;
        }
        if (kind == KIND_PULSE) {
            _tickPulse(now);
            return;
        }
        if (kind == KIND_RANGE) {
            _tickRange(now);
            return;
        }
        int level = digitalRead((uint8_t)_runSensor);
        if (!_seen || level != _candidate) {
            _seen = true;
            _candidate = level;
            _since = now;
            return;
        }
        if (now - _since < DEBOUNCE_MS) return;
        if (_haveLevel && level == _stable) return;
        _stable = level;
        _haveLevel = true;
        _apply(level);
    }

    bool pending() const { return _pending[0] != '\0'; }
    const char* message() const { return _pending; }
    void ack() { _pending[0] = '\0'; }

    bool moduleOn() const { return _moduleOn; }

    const char* sensorText() const {
        if (!allowedPin(_runSensor)) return "nenhum";
        if (sensorKind(_runSensorType) == KIND_DIGITAL) {
            if (!_haveLevel) return "aguardando";
            return _stable == HIGH ? "alto" : "baixo";
        }
        if (_failing || !_haveLevel) return _failing ? "falha" : "aguardando";
        return _sensorLine;
    }

    const char* moduleText() const {
        if (!_moduleIsOutput()) return "nenhum";
        uint8_t mk = moduleKind(_runModuleType);
        if (mk == MKIND_SERVO) {
            if (!_servoOk) return "falha";
            return _moduleLine;
        }
        if (mk == MKIND_PWM) return _moduleLine;
        return _moduleOn ? "ligado" : "desligado";
    }

    // Contagem de pulsos. O ISR só incrementa.
    static void IRAM_ATTR onPulse() { _pulses++; }

private:
    Config& _cfg;
    Logger& _log;
    bool    _seen;
    bool    _haveLevel;
    bool    _moduleOn;
    bool    _conditionOn;
    bool    _failing;
    bool    _haveRead;
    bool    _windowOpen;
    bool    _servoOk;
    int     _candidate;
    int     _stable;
    int     _hum;
    int     _temp;
    int     _shownAngle;
    unsigned long _since;
    unsigned long _lastRead;
    unsigned long _owSince;
    unsigned long _windowStart;
    uint8_t _owState;
    char    _pending[MSG_LEN];
    char    _sensorLine[40];
    char    _moduleLine[16];
    int8_t  _runSensor;
    int8_t  _runModule;
    bool    _runSensorHigh;
    bool    _runModuleHigh;
    bool    _runNotify;
    uint8_t _runSensorType;
    uint8_t _runModuleType;
    uint8_t _runMetric;
    bool    _runAbove;
    int16_t _runThreshold;
    uint8_t _runMargin;
    uint8_t _runAngleOn;
    uint8_t _runAngleOff;
    uint8_t _runDutyOn;
    uint8_t _runDutyOff;
    uint8_t _runManual;
    bool    _runToggle;
    bool    _runLinear;
    int16_t _runThresholdHi;
    uint8_t _shownDuty;
    bool    _edgeReady;
    Servo   _servo;

    struct _Cat {
        const char* label;
        uint8_t kind;
    };

    static const _Cat& _sensorAt(uint8_t type) {
        static const _Cat items[] = {
            {"Digital", KIND_DIGITAL},
            {"DHT11", KIND_DHT11},
            {"DHT22", KIND_DHT22},
            {"DS18B20", KIND_DS18},
            {"Pulsos (10 s)", KIND_PULSE},
            {"PIR", KIND_DIGITAL},
            {"Porta", KIND_DIGITAL},
            {"Chuva", KIND_DIGITAL},
            {"Chama", KIND_DIGITAL},
            {"Gas", KIND_DIGITAL},
            {"Solo", KIND_DIGITAL},
            {"Luz", KIND_DIGITAL},
            {"Som", KIND_DIGITAL},
            {"Boia", KIND_DIGITAL},
            {"Botao", KIND_DIGITAL},
            {"Ultrassom", KIND_RANGE},
        };
        static_assert(sizeof(items) / sizeof(items[0]) == SENSOR_COUNT, "sensores");
        if (type >= SENSOR_COUNT) return items[0];
        return items[type];
    }

    static const _Cat& _moduleAt(uint8_t type) {
        static const _Cat items[] = {
            {"Rele", MKIND_LEVEL},
            {"Servo", MKIND_SERVO},
            {"LED", MKIND_LEVEL},
            {"Buzzer", MKIND_LEVEL},
            {"Bomba", MKIND_LEVEL},
            {"Valvula", MKIND_LEVEL},
            {"Ventilador", MKIND_PWM},
            {"Motor", MKIND_PWM},
        };
        static_assert(sizeof(items) / sizeof(items[0]) == MODULE_COUNT, "modulos");
        if (type >= MODULE_COUNT) return items[0];
        return items[type];
    }

    static uint8_t _clampDuty(uint8_t duty) {
        return duty > 100 ? 100 : duty;
    }

    static int _dutyPwm(uint8_t duty) {
        return (int)duty * 1023 / 100;
    }

    static inline volatile uint32_t _pulses = 0;
    static constexpr uint8_t OW_IDLE = 0;
    static constexpr uint8_t OW_WAIT = 1;

    void _clear() {
        _seen = false;
        _haveLevel = false;
        _moduleOn = false;
        _conditionOn = false;
        _failing = false;
        _haveRead = false;
        _windowOpen = false;
        _servoOk = false;
        _candidate = HIGH;
        _stable = HIGH;
        _hum = 0;
        _temp = 0;
        _shownAngle = 0;
        _shownDuty = 0;
        _edgeReady = false;
        _since = 0;
        _lastRead = 0;
        _owSince = 0;
        _windowStart = 0;
        _owState = OW_IDLE;
        _pending[0] = '\0';
        _sensorLine[0] = '\0';
        _moduleLine[0] = '\0';
        _pulses = 0;
    }

    bool _moduleIsOutput() const {
        return allowedPin(_runSensor) &&
               allowedPin(_runModule) &&
               _runModule != _runSensor;
    }

    void _detachSensor() {
        if (sensorKind(_runSensorType) == KIND_PULSE && allowedPin(_runSensor))
            detachInterrupt((uint8_t)_runSensor);
    }

    // GPIO0 e GPIO2 têm de ficar altos no próximo boot.
    void _idleStrappingPin() {
        if (_runModule == 0 || _runModule == 2)
            pinMode((uint8_t)_runModule, INPUT_PULLUP);
    }

    // Segura a saída no nível desligado. Entrada com pull-up ligaria
    // um módulo que arma em alto. No servo e no PWM, GPIO0 e GPIO2
    // voltam ao pull-up para continuarem altos no próximo boot.
    void _releaseOutput() {
        if (!_moduleIsOutput()) return;
        uint8_t mk = moduleKind(_runModuleType);
        if (mk == MKIND_SERVO) {
            if (_servoOk) _servo.detach();
            _servoOk = false;
            _idleStrappingPin();
            _moduleOn = false;
            return;
        }
        if (mk == MKIND_PWM) {
            analogWrite((uint8_t)_runModule, 0);
            _idleStrappingPin();
            _moduleOn = false;
            return;
        }
        digitalWrite((uint8_t)_runModule, _runModuleHigh ? LOW : HIGH);
        _moduleOn = false;
    }

    void _writeModule(bool on) {
        if (!_moduleIsOutput()) {
            _moduleOn = false;
            return;
        }
        uint8_t mk = moduleKind(_runModuleType);
        if (mk == MKIND_SERVO) {
            if (!_servoOk) {
                _moduleOn = false;
                return;
            }
            _moduleOn = on;
            _shownAngle = on ? _runAngleOn : _runAngleOff;
            _servo.write(_shownAngle);
            snprintf(_moduleLine, sizeof(_moduleLine), "servo %d", _shownAngle);
            return;
        }
        if (mk == MKIND_PWM) {
            _moduleOn = on;
            uint8_t duty = on ? _runDutyOn : _runDutyOff;
            _shownDuty = duty;
            analogWrite((uint8_t)_runModule, _dutyPwm(duty));
            snprintf(_moduleLine, sizeof(_moduleLine), "pwm %u%%", duty);
            return;
        }
        _moduleOn = on;
        bool high = on ? _runModuleHigh : !_runModuleHigh;
        digitalWrite((uint8_t)_runModule, high ? HIGH : LOW);
    }

    void _moduleSuffix(char* buf, size_t n) const {
        buf[0] = '\0';
        if (!_moduleIsOutput()) return;
        uint8_t mk = moduleKind(_runModuleType);
        if (mk == MKIND_SERVO) {
            if (!_servoOk) snprintf(buf, n, " servo falha");
            else snprintf(buf, n, " servo %d", _shownAngle);
            return;
        }
        if (mk == MKIND_PWM) {
            snprintf(buf, n, " pwm %u%%", _shownDuty);
            return;
        }
        snprintf(buf, n, " modulo %s", _moduleOn ? "ligado" : "desligado");
    }

    bool _outputLocked() const { return _runManual != MANUAL_AUTO; }

    bool _proportional() const {
        return _runLinear && moduleKind(_runModuleType) == MKIND_PWM;
    }

    void _command(bool on) {
        if (_outputLocked()) return;
        _writeModule(on);
    }

    void _commandDuty(uint8_t duty) {
        if (_outputLocked()) return;
        if (!_moduleIsOutput()) {
            _moduleOn = false;
            return;
        }
        _shownDuty = duty;
        _moduleOn = duty != _runDutyOff;
        analogWrite((uint8_t)_runModule, _dutyPwm(duty));
        snprintf(_moduleLine, sizeof(_moduleLine), "pwm %u%%", duty);
    }

    // Limite alto acima do limite: interpola. Faixa vazia vira dois patamares.
    uint8_t _mapDuty(int value, bool tenths) const {
        int scale = tenths ? 10 : 1;
        int lo = (int)_runThreshold * scale;
        int hi = (int)_runThresholdHi * scale;
        if (hi <= lo) return value >= lo ? _runDutyOn : _runDutyOff;
        if (value <= lo) return _runDutyOff;
        if (value >= hi) return _runDutyOn;
        int span = hi - lo;
        int delta = (int)_runDutyOn - (int)_runDutyOff;
        return (uint8_t)((int)_runDutyOff + delta * (value - lo) / span);
    }

    void _apply(int level) {
        bool sensorOn = (level == HIGH) == _runSensorHigh;
        if (_runToggle) {
            if (_edgeReady && sensorOn) _command(!_moduleOn);
            _edgeReady = true;
        } else {
            _command(sensorOn);
        }
        const char* name = level == HIGH ? "alto" : "baixo";
        if (_runNotify) {
            char suffix[24];
            _moduleSuffix(suffix, sizeof(suffix));
            snprintf(_pending, sizeof(_pending), "[sensor] GPIO%d %s%s",
                     _runSensor, name, suffix);
            _log.log("%s", _pending);
        } else {
            _pending[0] = '\0';
            _log.log("IoT: GPIO%d %s", _runSensor, name);
        }
    }

    void _fail() {
        if (_failing) return;
        _failing = true;
        _log.log("IoT: leitura falhou");
    }

    void _fmtTenths(char* b, size_t n, int v) const {
        if (v < 0) {
            v = -v;
            snprintf(b, n, "-%d.%d", v / 10, v % 10);
            return;
        }
        snprintf(b, n, "%d.%d", v / 10, v % 10);
    }

    // value em décimos, exceto pulsos. A margem evita o relé oscilar.
    bool _rule(int value, bool tenths) const {
        int scale = tenths ? 10 : 1;
        int limit = (int)_runThreshold * scale;
        int margin = (int)_runMargin * scale;
        if (_haveLevel && _conditionOn) {
            if (_runAbove) return value >= limit - margin;
            return value <= limit + margin;
        }
        if (_runAbove) return value >= limit;
        return value <= limit;
    }

    void _accept(int value, bool tenths) {
        _failing = false;
        uint8_t kind = sensorKind(_runSensorType);
        if (kind == KIND_PULSE) {
            snprintf(_sensorLine, sizeof(_sensorLine), "pulsos %d", value);
        } else if (kind == KIND_DS18) {
            char temp[16];
            _fmtTenths(temp, sizeof(temp), value);
            snprintf(_sensorLine, sizeof(_sensorLine), "temp %sC", temp);
        } else if (kind == KIND_RANGE) {
            snprintf(_sensorLine, sizeof(_sensorLine), "dist %d cm", value);
        } else {
            char hum[16];
            char temp[16];
            _fmtTenths(hum, sizeof(hum), _hum);
            _fmtTenths(temp, sizeof(temp), _temp);
            snprintf(_sensorLine, sizeof(_sensorLine), "umid %s%% temp %sC", hum, temp);
        }
        bool changed;
        if (_proportional()) {
            uint8_t duty = _mapDuty(value, tenths);
            changed = !_haveLevel || duty != _shownDuty;
            if (changed) _commandDuty(duty);
            _conditionOn = _shownDuty != _runDutyOff;
        } else {
            bool on = _rule(value, tenths);
            changed = !_haveLevel || on != _conditionOn;
            if (changed) _command(on);
            _conditionOn = on;
        }
        _haveLevel = true;
        if (!_runNotify) {
            _pending[0] = '\0';
            if (changed) _log.log("IoT: GPIO%d %s", _runSensor, _sensorLine);
            return;
        }
        char suffix[24];
        _moduleSuffix(suffix, sizeof(suffix));
        snprintf(_pending, sizeof(_pending), "[sensor] GPIO%d %s%s",
                 _runSensor, _sensorLine, suffix);
        if (changed) _log.log("%s", _pending);
    }

    bool _waitLevel(int level, unsigned long timeoutUs) {
        unsigned long start = micros();
        while (digitalRead((uint8_t)_runSensor) != level) {
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

    bool _readDht(uint8_t data[5]) {
        uint8_t pin = (uint8_t)_runSensor;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        if (sensorKind(_runSensorType) == KIND_DHT11) delay(18);
        else delayMicroseconds(1100);
        digitalWrite(pin, HIGH);
        delayMicroseconds(40);
        pinMode(pin, INPUT_PULLUP);

        noInterrupts();
        bool ok = _dhtBits(data);
        interrupts();
        if (!ok) return false;
        uint8_t sum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
        if (sum != data[4]) return false;
        return true;
    }

    void _decodeDht(const uint8_t d[5]) {
        if (sensorKind(_runSensorType) == KIND_DHT11) {
            _hum = d[0] * 10 + d[1];
            _temp = d[2] * 10 + d[3];
            return;
        }
        _hum = (d[0] << 8) | d[1];
        int t = ((d[2] & 0x7F) << 8) | d[3];
        if (d[2] & 0x80) t = -t;
        _temp = t;
    }

    void _tickDht(unsigned long now) {
        if (_haveRead && now - _lastRead < READ_GAP_MS) return;
        _lastRead = now;
        _haveRead = true;
        uint8_t data[5];
        if (!_readDht(data)) {
            _fail();
            return;
        }
        _decodeDht(data);
        int metric = _runMetric == 1 ? _temp : _hum;
        _accept(metric, true);
    }

    bool _owReset() {
        uint8_t pin = (uint8_t)_runSensor;
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
        uint8_t pin = (uint8_t)_runSensor;
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
        uint8_t pin = (uint8_t)_runSensor;
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

    bool _owStart() {
        if (!_owReset()) return false;
        _owWriteByte(0xCC);
        _owWriteByte(0x44);
        return true;
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
            _accept(tenths, true);
            return;
        }
        if (_haveRead && now - _lastRead < READ_GAP_MS) return;
        _lastRead = now;
        _haveRead = true;
        if (!_owStart()) {
            _fail();
            return;
        }
        _owState = OW_WAIT;
        _owSince = now;
    }

    // HC-SR04 com trig e echo no mesmo pino. A distância sai em cm.
    bool _readRange(int* cm) {
        uint8_t pin = (uint8_t)_runSensor;
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
        _accept((int)n, false);
    }
};
