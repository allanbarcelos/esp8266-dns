// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <algorithm>
#include <ESP8266WiFi.h>
#include "Config.h"
#include "Logger.h"
#include "IoTService.h"

// MQTT 3.1.1 on the local network (Home Assistant, Node-RED, scripts).
//
// Own minimal client, QoS 0 only: a library would cost 4 KB more flash. No
// TLS: the device's only TLS session is already shared by DNS, ntfy and
// OTA. Use a broker on the local network, with a user and password; anyone
// who can publish to the broker controls the actuator.
//
// Topics, with <id> = chip ID in hexadecimal:
//   esp8266iot/<id>/state  state as JSON (the same as the API), retained;
//                          sent on connect, on change and every TELE_MS
//                          even without a change (Tasmota's TelePeriod): a
//                          lost message does not leave HA without a state
//   esp8266iot/<id>/avail  online, or offline through the will when the
//                          connection drops; retained
//   esp8266iot/<id>/cmd    commands, with the same text as the panel and
//                          the API. A retained message is ignored: it
//                          would repeat on every connection.
//
// Plus Home Assistant discovery (below): the device shows up on its own,
// with the entities of the configured sensor and actuator.
class MqttService {
public:
    MqttService(Config& cfg, Logger& log, IoTService& iot, WiFiClient& net)
        : _cfg(cfg), _log(log), _iot(iot), _net(net) {
        _base[0] = '\0';
    }

    // OTA download: the connection closes and frees the heap
    void hold(bool on) {
        if (on) _close();
        _held = on;
    }

    // Configuration changed: reconnect now, with the new data
    void reconnect() {
        _close();
        _waiting = false;
        _backoff = BACKOFF_MIN_S;
    }

    void tick(unsigned long now) {
        if (_held || !_cfg.mqtt_host[0] || WiFi.status() != WL_CONNECTED) {
            _close();
            return;
        }
        if (_state == ST_IDLE) {
            if (_waiting && (long)(now - _next) < 0) return;
            _connect(now);
            return;
        }
        // Reads what arrived without waiting, with a limit per tick
        for (size_t i = 0; i < READ_PER_TICK && _net.available() > 0; i++) {
            _lastIn = now;
            _feed((uint8_t)_net.read(), now);
            if (_state == ST_IDLE) return;
        }
        if (!_net.connected()) {
            _log.log(PSTR("MQTT: connection closed"));
            _fail(now);
            return;
        }
        if (_state == ST_CONNECTING) {
            if (now - _lastIn > CONNACK_MS) {
                _log.log(PSTR("MQTT: broker not answering"));
                _fail(now);
            }
            return;
        }
        if (now - _lastIn > KEEPALIVE_S * 1500UL) {   // 1.5 × keepalive with nothing
            _log.log(PSTR("MQTT: broker stalled"));
            _fail(now);
            return;
        }
        if (now - _lastOut >= KEEPALIVE_S * 500UL) {   // PINGREQ halfway
            static const uint8_t PING[] = {0xC0, 0x00};
            _write(PING, sizeof(PING), now);
        }
        if (_shapeNow() != _shape) {                   // module changed on the panel
            _announce(now);
            _publishState(true, now);
        }
        if (now - _lastState >= STATE_MS) {
            _lastState = now;
            _publishState(now - _lastSent >= TELE_MS, now);
        }
    }

    // For the panel
    const char* status() const {
        if (!_cfg.mqtt_host[0]) return "off";
        if (_held) return "paused";
        return _state == ST_UP ? "connected" : "connecting";
    }

    const char* base() {
        if (!_base[0]) snprintf_P(_base, sizeof(_base), PSTR("esp8266iot/%06x"), ESP.getChipId());
        return _base;
    }

    // "host" or "host:port" (1883 if omitted); no spaces
    static bool parseBroker(const char* s, char* host, size_t hostLen, uint16_t& port) {
        const char* colon = strchr(s, ':');
        size_t n = colon ? (size_t)(colon - s) : strlen(s);
        if (n == 0 || n >= hostLen || memchr(s, ' ', n)) return false;
        memcpy(host, s, n);
        host[n] = '\0';
        port = 1883;
        if (!colon) return true;
        const char* p = colon + 1;
        uint32_t v = 0;
        do {
            if (*p < '0' || *p > '9') return false;
            v = v * 10 + (uint32_t)(*p - '0');
            if (v > 65535) return false;
        } while (*++p);
        port = (uint16_t)v;
        return v != 0;
    }

    static constexpr uint8_t       KEEPALIVE_S   = 60;
    static constexpr unsigned long CONNECT_MS    = 2000UL;  // TCP and DNS: blocks the loop up to this
    static constexpr unsigned long CONNACK_MS    = 10000UL;
    static constexpr unsigned long STATE_MS      = 1000UL;  // a changed state goes out within 1 s
    static constexpr unsigned long TELE_MS       = 300000UL; // and without a change, every 5 min
    static constexpr uint16_t      BACKOFF_MIN_S = 5;
    static constexpr uint16_t      BACKOFF_MAX_S = 300;
    static constexpr size_t        READ_PER_TICK = 256;
    static constexpr size_t        RX_LEN        = 80;      // command topic + command
    static constexpr size_t        CMD_LEN       = 40;

private:
    enum : uint8_t { ST_IDLE, ST_CONNECTING, ST_UP };
    enum : uint8_t { RX_HEADER, RX_LENGTH, RX_BODY };

    Config&      _cfg;
    Logger&      _log;
    IoTService&  _iot;
    WiFiClient&  _net;

    char          _base[24];
    uint8_t       _state   = ST_IDLE;
    bool          _held    = false;
    bool          _waiting = false;
    unsigned long _next = 0, _lastIn = 0, _lastOut = 0, _lastState = 0, _lastSent = 0;
    uint16_t      _backoff = BACKOFF_MIN_S;
    uint32_t      _sum = 0;              // state already published (FNV-1a)
    uint32_t      _shape = 0;            // module announced in discovery
    size_t        _count = 0;            // discovery: measured size
    bool          _counting = false;

    // Incoming packet: header, length (up to 4 bytes) and body
    uint8_t  _rxState = RX_HEADER, _rxHeader = 0, _rxShift = 0;
    uint32_t _rxLen = 0, _rxGot = 0;
    char     _rx[RX_LEN] = {};

    void _close() {
        if (_state != ST_IDLE) _net.stop();
        _state = ST_IDLE;
    }

    void _fail(unsigned long now) {
        _close();
        _waiting = true;
        _next = now + _backoff * 1000UL;
        _backoff = _backoff < BACKOFF_MAX_S / 2 ? _backoff * 2 : BACKOFF_MAX_S;
    }

    void _connect(unsigned long now) {
        char host[sizeof(_cfg.mqtt_host)];
        uint16_t port;
        if (!parseBroker(_cfg.mqtt_host, host, sizeof(host), port)) {
            _log.log(PSTR("MQTT: invalid broker"));
            _fail(now);
            return;
        }
        _net.setTimeout(CONNECT_MS);
        if (!_net.connect(host, port)) {
            _log.log(PSTR("MQTT: cannot connect to %s"), host);
            _fail(now);
            return;
        }
        _state = ST_CONNECTING;
        _rxState = RX_HEADER;
        _lastIn = now;

        // CONNECT: "MQTT" level 4, clean session, retained "offline" will
        char id[24], will[32];
        snprintf_P(id, sizeof(id), PSTR("esp8266iot-%s"), base() + 11);
        snprintf_P(will, sizeof(will), PSTR("%s/avail"), _base);
        const char* user = _cfg.mqtt_user;
        const char* pass = user[0] ? _cfg.mqtt_pass : "";   // a password needs a user
        uint8_t flags = 0x02 | 0x04 | 0x20 | (user[0] ? 0x80 : 0) | (pass[0] ? 0x40 : 0);
        size_t len = 10 + 2 + strlen(id) + 2 + strlen(will) + 2 + 7
                   + (user[0] ? 2 + strlen(user) : 0) + (pass[0] ? 2 + strlen(pass) : 0);
        _header(0x10, len, now);
        const uint8_t vh[10] = {0, 4, 'M', 'Q', 'T', 'T', 4, flags, 0, KEEPALIVE_S};
        _write(vh, sizeof(vh), now);
        _string(id, now);
        _string(will, now);
        _string("offline", now);
        if (user[0]) _string(user, now);
        if (pass[0]) _string(pass, now);
    }

    void _online(unsigned long now) {
        _state = ST_UP;
        _backoff = BACKOFF_MIN_S;
        _waiting = false;
        _log.log(PSTR("MQTT: connected"));
        _publish("avail", "online", 6, now);

        // SUBSCRIBE (id 1, QoS 0) to the command topic
        char topic[32];
        size_t tl = (size_t)snprintf_P(topic, sizeof(topic), PSTR("%s/cmd"), _base);
        _header(0x82, 2 + 2 + tl + 1, now);
        static const uint8_t ID[] = {0, 1};
        _write(ID, sizeof(ID), now);
        _string(topic, now);
        static const uint8_t QOS0[] = {0};
        _write(QOS0, sizeof(QOS0), now);

        _announce(now);
        _publishState(true, now);
    }

    void _publishState(bool force, unsigned long now) {
        char json[384];
        size_t n = std::min(_iot.stateJson(json, sizeof(json)), sizeof(json) - 1);
        uint32_t sum = 2166136261UL;
        for (size_t i = 0; i < n; i++) sum = (sum ^ (uint8_t)json[i]) * 16777619UL;
        if (!force && sum == _sum) return;
        _sum = sum;
        _lastSent = now;
        _publish("state", json, n, now);
    }

    // Everything the device publishes is retained: a later subscriber gets it right away
    void _publish(const char* sub, const char* body, size_t n, unsigned long now) {
        char topic[32];
        size_t tl = (size_t)snprintf_P(topic, sizeof(topic), PSTR("%s/%s"), _base, sub);
        _header(0x31, 2 + tl + n, now);
        _string(topic, now);
        _write((const uint8_t*)body, n, now);
    }

    void _header(uint8_t type, size_t len, unsigned long now) {
        uint8_t h[5];
        size_t k = 0;
        h[k++] = type;
        do {
            uint8_t b = len % 128;
            len /= 128;
            h[k++] = b | (len ? 0x80 : 0);
        } while (len);
        _write(h, k, now);
    }

    void _string(const char* s, unsigned long now) {
        size_t n = strlen(s);
        const uint8_t len[2] = {(uint8_t)(n >> 8), (uint8_t)n};
        _write(len, 2, now);
        _write((const uint8_t*)s, n, now);
    }

    void _write(const uint8_t* p, size_t n, unsigned long now) {
        _net.write(p, n);
        _lastOut = now;
    }

    // Packet from the broker, byte by byte. A body larger than _rx is read and dropped.
    void _feed(uint8_t c, unsigned long now) {
        if (_rxState == RX_HEADER) {
            _rxHeader = c;
            _rxLen = 0;
            _rxShift = 0;
            _rxState = RX_LENGTH;
            return;
        }
        if (_rxState == RX_LENGTH) {
            _rxLen |= (uint32_t)(c & 0x7F) << _rxShift;
            _rxShift += 7;
            if (c & 0x80) {
                if (_rxShift < 28) return;
                _log.log(PSTR("MQTT: invalid packet"));   // length with more than 4 bytes
                _fail(now);
                return;
            }
            _rxGot = 0;
            _rxState = RX_BODY;
            if (_rxLen) return;
        } else {
            if (_rxGot < RX_LEN) _rx[_rxGot] = (char)c;
            if (++_rxGot < _rxLen) return;
        }
        _rxState = RX_HEADER;
        _packet(now);
    }

    void _packet(unsigned long now) {
        uint8_t type = _rxHeader >> 4;
        if (type == 2) {                                     // CONNACK
            if (_state != ST_CONNECTING) return;
            uint8_t code = _rxLen >= 2 ? (uint8_t)_rx[1] : 0xFF;
            if (code == 0) {
                _online(now);
                return;
            }
            // 4: user or password; 5: not authorized
            _log.log(PSTR("MQTT: broker refused (%u)"), code);
            _fail(now);
        } else if (type == 3 && _state == ST_UP) {           // PUBLISH
            _command(now);
        }
        // SUBACK, PINGRESP: _lastIn already marked activity
    }

    // ── Home Assistant discovery ─────────────────────────────────────────────
    //
    // One message per device (device discovery, HA 2024.12+), retained, at
    // homeassistant/device/esp8266iot_<id>/config. It lists every possible
    // entity: those the configured module lacks go with the platform only,
    // which removes them from HA. So changing the module on the panel
    // republishes and cleans up what was left. The text lives in flash and
    // goes out in pieces; a first pass only measures the size.

    struct Entity {
        char  key;
        PGM_P platform;
        PGM_P body;     // after uniq_id
    };

    bool _has(char key) const {
        uint8_t k = _iot.sensorKind();
        uint8_t c = _iot.capability();
        switch (key) {
            case 't': return iot::tempAndHumidity(k) || k == iot::K_DS18;
            case 'h': return iot::tempAndHumidity(k);
            case 'd': return k == iot::K_RANGE;
            case 'n': return k == iot::K_PULSE;
            case 'x': return k == iot::K_LUX;
            case 'b': return k == iot::K_DIGITAL;
            case 's': return c == iot::C_ONOFF;
            case 'l': return c == iot::C_LEVEL;
            case 'c': return c == iot::C_COLOR;
            default:  return c == iot::C_POSITION;   // 'g'
        }
    }

    // Configured module: changes when the panel switches the sensor or the actuator
    uint32_t _shapeNow() const {
        return (uint32_t)(uintptr_t)_iot.sensorDef() * 31u + (uint32_t)(uintptr_t)_iot.actuatorDef();
    }

    void _announce(unsigned long now) {
        _shape = _shapeNow();
        char topic[64];
        size_t tl = (size_t)snprintf_P(topic, sizeof(topic),
                                       PSTR("homeassistant/device/esp8266iot_%s/config"), base() + 11);
        _count = 0;
        _counting = true;
        _discovery();
        _header(0x31, 2 + tl + _count, now);
        _string(topic, now);
        _counting = false;
        _discovery();
    }

    void _out(const char* s) {
        size_t n = strlen(s);
        if (_counting) _count += n;
        else _net.write((const uint8_t*)s, n);
    }

    void _outP(PGM_P s) {
        size_t n = strlen_P(s);
        if (_counting) _count += n;
        else _net.write_P(s, n);
    }

    void _discovery() {
        static const char P_SENSOR[] PROGMEM = "sensor";
        static const char P_BINARY[] PROGMEM = "binary_sensor";
        static const char P_SWITCH[] PROGMEM = "switch";
        static const char P_LIGHT[]  PROGMEM = "light";
        static const char P_NUMBER[] PROGMEM = "number";
        static const char STATE_ON[] PROGMEM =
            ",\"name\":null,\"stat_tpl\":\"{{'on' if value_json.actuator.on else 'off'}}\","
            "\"cmd_off_tpl\":\"off\",\"schema\":\"template\"";
        // On the stack: PSTR cannot initialize a static. A sensor with no
        // reading yet has no field in the JSON: |default avoids the undefined
        // variable warning in the HA log (the state stays unknown).
        const Entity ENTITIES[] = {
            {'t', P_SENSOR, PSTR(",\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\xc2\xb0" "C\","
                                 "\"stat_cla\":\"measurement\",\"val_tpl\":\"{{value_json.sensor.temperature|default(none)}}\"")},
            {'h', P_SENSOR, PSTR(",\"dev_cla\":\"humidity\",\"unit_of_meas\":\"%\","
                                 "\"stat_cla\":\"measurement\",\"val_tpl\":\"{{value_json.sensor.humidity|default(none)}}\"")},
            {'d', P_SENSOR, PSTR(",\"dev_cla\":\"distance\",\"unit_of_meas\":\"cm\","
                                 "\"stat_cla\":\"measurement\",\"val_tpl\":\"{{value_json.sensor.distance|default(none)}}\"")},
            {'n', P_SENSOR, PSTR(",\"name\":\"Pulses\",\"stat_cla\":\"measurement\","
                                 "\"val_tpl\":\"{{value_json.sensor.pulses|default(none)}}\"")},
            {'x', P_SENSOR, PSTR(",\"dev_cla\":\"illuminance\",\"unit_of_meas\":\"lx\","
                                 "\"stat_cla\":\"measurement\",\"val_tpl\":\"{{value_json.sensor.lux|default(none)}}\"")},
            {'b', P_BINARY, PSTR(",\"name\":\"Sensor\","
                                 "\"val_tpl\":\"{{'ON' if value_json.sensor.active|default(0) else 'OFF'}}\"")},
            {'s', P_SWITCH, PSTR(",\"name\":null,\"pl_on\":\"on\",\"pl_off\":\"off\","
                                 "\"val_tpl\":\"{{'on' if value_json.actuator.on else 'off'}}\"")},
            {'l', P_LIGHT,  PSTR(",\"cmd_on_tpl\":\"{%if brightness is defined%}level {{(brightness/2.55)|round|int}}"
                                 "{%else%}on{%endif%}\","
                                 "\"bri_tpl\":\"{{(value_json.actuator.level*2.55)|round|int}}\"")},
            {'c', P_LIGHT,  PSTR(",\"cmd_on_tpl\":\"{%if red is defined%}color #{{'%02x%02x%02x'|format(red,green,blue)}}"
                                 "{%elif brightness is defined%}level {{(brightness/2.55)|round|int}}"
                                 "{%else%}on{%endif%}\","
                                 "\"bri_tpl\":\"{{(value_json.actuator.level*2.55)|round|int}}\","
                                 "\"r_tpl\":\"{{value_json.actuator.color[1:3]|int(base=16)}}\","
                                 "\"g_tpl\":\"{{value_json.actuator.color[3:5]|int(base=16)}}\","
                                 "\"b_tpl\":\"{{value_json.actuator.color[5:7]|int(base=16)}}\"")},
            {'g', P_NUMBER, PSTR(",\"name\":null,\"min\":0,\"max\":180,\"unit_of_meas\":\"\xc2\xb0\","
                                 "\"cmd_tpl\":\"angle {{value}}\",\"val_tpl\":\"{{value_json.actuator.angle}}\"")},
        };

        const char* id = base() + 11;
        _outP(PSTR("{\"dev\":{\"ids\":\"esp8266iot_"));
        _out(id);
        _outP(PSTR("\",\"name\":\""));
        // The panel rejects " and \ in the name: it goes unescaped
        if (_cfg.dev_name[0]) _out(_cfg.dev_name);
        else _outP(PSTR("ESP8266 IoT"));
        _outP(PSTR("\",\"mf\":\"esp8266-dns\",\"sw\":\""));
        _out(firmware_version);
        _outP(PSTR("\"},\"o\":{\"name\":\"esp8266-dns\"},\"stat_t\":\""));
        _out(_base);
        _outP(PSTR("/state\",\"avty_t\":\""));
        _out(_base);
        _outP(PSTR("/avail\",\"cmd_t\":\""));
        _out(_base);
        _outP(PSTR("/cmd\",\"cmps\":{"));
        for (const Entity& e : ENTITIES) {
            const char head[8] = {',', '"', e.key, '"', ':', '{', '\0'};
            _out(e.key == 't' ? head + 1 : head);
            _outP(PSTR("\"p\":\""));
            _outP(e.platform);
            _outP(PSTR("\""));
            if (_has(e.key)) {
                _outP(PSTR(",\"uniq_id\":\"esp8266iot_"));
                _out(id);
                const char tail[4] = {'_', e.key, '"', '\0'};
                _out(tail);
                if (e.key == 'b') _binaryClass();
                if (e.platform == P_LIGHT) _outP(STATE_ON);
                _outP(e.body);
            }
            _outP(PSTR("}"));
        }
        _outP(PSTR("}}"));
    }

    // PIR is motion and contact is a door; button and digital have no class
    // Home Assistant device class of each named digital sensor
    void _binaryClass() {
        static const char* const CLASSES[][2] = {{"pir", "motion"}, {"contact", "door"}, {"radar", "occupancy"},
                                                 {"leak", "moisture"}, {"gas", "gas"}, {"rain", "moisture"}};
        const char* slug = _iot.sensorDef()->slug;
        for (const auto& c : CLASSES) {
            if (strcmp(slug, c[0])) continue;
            _outP(PSTR(",\"dev_cla\":\""));
            _out(c[1]);
            _outP(PSTR("\""));
        }
    }

    void _command(unsigned long now) {
        if (_rxHeader & 0x01) {
            _log.log(PSTR("MQTT: retained command ignored"));
            return;
        }
        if ((_rxHeader & 0x06) || _rxLen > RX_LEN || _rxLen < 2) return;   // QoS 0 only, whole in _rx
        size_t tl = ((uint8_t)_rx[0] << 8) | (uint8_t)_rx[1];
        if (2 + tl > _rxLen) return;
        size_t n = _rxLen - 2 - tl;
        char cmd[CMD_LEN];
        if (n >= sizeof(cmd)) n = sizeof(cmd) - 1;
        memcpy(cmd, _rx + 2 + tl, n);
        cmd[n] = '\0';
        uint8_t r = _iot.command(cmd, now);
        _log.log(PSTR("MQTT: command \"%.32s\"%s"), cmd, r == IoTService::OK ? "" : " refused");
        _publishState(true, now);
    }
};
