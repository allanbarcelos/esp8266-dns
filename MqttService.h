// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <algorithm>
#include <ESP8266WiFi.h>
#include "Config.h"
#include "Logger.h"
#include "IoTService.h"

// MQTT 3.1.1 na rede local (Home Assistant, Node-RED, scripts).
//
// Cliente próprio e mínimo, só QoS 0: uma biblioteca custaria 4 KB de
// flash a mais. Sem TLS: a única sessão TLS do aparelho já é disputada pelo
// DNS, pelo ntfy e pelo OTA. Use um broker na rede local, com usuário e
// senha; quem puder publicar no broker comanda o atuador.
//
// Tópicos, com <id> = chip ID em hexadecimal:
//   esp8266iot/<id>/state  estado em JSON (o mesmo da API), retido; sai na
//                          conexão e quando muda
//   esp8266iot/<id>/avail  online, ou offline pelo testamento quando a
//                          conexão cai; retido
//   esp8266iot/<id>/cmd    comandos, com o mesmo texto do painel e da API.
//                          Mensagem retida é ignorada: seria repetida a
//                          cada conexão.
class MqttService {
public:
    MqttService(Config& cfg, Logger& log, IoTService& iot, WiFiClient& net)
        : _cfg(cfg), _log(log), _iot(iot), _net(net) {
        _base[0] = '\0';
    }

    // Download do OTA: a conexão fecha e libera a heap
    void hold(bool on) {
        if (on) _close();
        _held = on;
    }

    // Configuração mudou: reconecta já, com os dados novos
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
        // Lê o que chegou sem esperar, com limite por tick
        for (size_t i = 0; i < READ_PER_TICK && _net.available() > 0; i++) {
            _lastIn = now;
            _feed((uint8_t)_net.read(), now);
            if (_state == ST_IDLE) return;
        }
        if (!_net.connected()) {
            _log.log(PSTR("MQTT: conexao encerrada"));
            _fail(now);
            return;
        }
        if (_state == ST_CONNECTING) {
            if (now - _lastIn > CONNACK_MS) {
                _log.log(PSTR("MQTT: broker sem resposta"));
                _fail(now);
            }
            return;
        }
        if (now - _lastIn > KEEPALIVE_S * 1500UL) {   // 1,5 × keepalive sem nada
            _log.log(PSTR("MQTT: broker parado"));
            _fail(now);
            return;
        }
        if (now - _lastOut >= KEEPALIVE_S * 500UL) {   // PINGREQ na metade
            static const uint8_t PING[] = {0xC0, 0x00};
            _write(PING, sizeof(PING), now);
        }
        if (now - _lastState >= STATE_MS) {
            _lastState = now;
            _publishState(false, now);
        }
    }

    // Para o painel
    const char* status() const {
        if (!_cfg.mqtt_host[0]) return "desligado";
        if (_held) return "pausado";
        return _state == ST_UP ? "conectado" : "conectando";
    }

    const char* base() {
        if (!_base[0]) snprintf_P(_base, sizeof(_base), PSTR("esp8266iot/%06x"), ESP.getChipId());
        return _base;
    }

    // "host" ou "host:porta" (1883 se omitida); sem espaço
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
    static constexpr unsigned long CONNECT_MS    = 2000UL;  // TCP e DNS: trava o laço até isso
    static constexpr unsigned long CONNACK_MS    = 10000UL;
    static constexpr unsigned long STATE_MS      = 1000UL;  // estado mudado sai em até 1 s
    static constexpr uint16_t      BACKOFF_MIN_S = 5;
    static constexpr uint16_t      BACKOFF_MAX_S = 300;
    static constexpr size_t        READ_PER_TICK = 256;
    static constexpr size_t        RX_LEN        = 80;      // tópico de comando + comando
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
    unsigned long _next = 0, _lastIn = 0, _lastOut = 0, _lastState = 0;
    uint16_t      _backoff = BACKOFF_MIN_S;
    uint32_t      _sum = 0;              // estado já publicado (FNV-1a)

    // Pacote que chega: cabeçalho, tamanho (até 4 bytes) e corpo
    uint8_t  _rxState = RX_HEADER, _rxHeader = 0, _rxShift = 0;
    uint32_t _rxLen = 0, _rxGot = 0;
    char     _rx[RX_LEN];

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
            _log.log(PSTR("MQTT: broker invalido"));
            _fail(now);
            return;
        }
        _net.setTimeout(CONNECT_MS);
        if (!_net.connect(host, port)) {
            _log.log(PSTR("MQTT: sem conexao com %s"), host);
            _fail(now);
            return;
        }
        _state = ST_CONNECTING;
        _rxState = RX_HEADER;
        _lastIn = now;

        // CONNECT: "MQTT" nível 4, sessão limpa, testamento "offline" retido
        char id[24], will[32];
        snprintf_P(id, sizeof(id), PSTR("esp8266iot-%s"), base() + 11);
        snprintf_P(will, sizeof(will), PSTR("%s/avail"), _base);
        const char* user = _cfg.mqtt_user;
        const char* pass = user[0] ? _cfg.mqtt_pass : "";   // senha exige usuário
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
        _log.log(PSTR("MQTT: conectado"));
        _publish("avail", "online", 6, now);

        // SUBSCRIBE (id 1, QoS 0) ao tópico de comandos
        char topic[32];
        size_t tl = (size_t)snprintf_P(topic, sizeof(topic), PSTR("%s/cmd"), _base);
        _header(0x82, 2 + 2 + tl + 1, now);
        static const uint8_t ID[] = {0, 1};
        _write(ID, sizeof(ID), now);
        _string(topic, now);
        static const uint8_t QOS0[] = {0};
        _write(QOS0, sizeof(QOS0), now);

        _publishState(true, now);
    }

    void _publishState(bool force, unsigned long now) {
        char json[384];
        size_t n = std::min(_iot.stateJson(json, sizeof(json)), sizeof(json) - 1);
        uint32_t sum = 2166136261UL;
        for (size_t i = 0; i < n; i++) sum = (sum ^ (uint8_t)json[i]) * 16777619UL;
        if (!force && sum == _sum) return;
        _sum = sum;
        _publish("state", json, n, now);
    }

    // Tudo o que o aparelho publica é retido: quem assina depois já recebe
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

    // Pacote do broker, byte a byte. Corpo maior que _rx é lido e descartado.
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
                _log.log(PSTR("MQTT: pacote invalido"));   // tamanho com mais de 4 bytes
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
            // 4: usuário ou senha; 5: sem autorização
            _log.log(PSTR("MQTT: broker recusou (%u)"), code);
            _fail(now);
        } else if (type == 3 && _state == ST_UP) {           // PUBLISH
            _command(now);
        }
        // SUBACK, PINGRESP: o _lastIn já marcou atividade
    }

    void _command(unsigned long now) {
        if (_rxHeader & 0x01) {
            _log.log(PSTR("MQTT: comando retido ignorado"));
            return;
        }
        if ((_rxHeader & 0x06) || _rxLen > RX_LEN || _rxLen < 2) return;   // só QoS 0, inteiro no _rx
        size_t tl = ((uint8_t)_rx[0] << 8) | (uint8_t)_rx[1];
        if (2 + tl > _rxLen) return;
        size_t n = _rxLen - 2 - tl;
        char cmd[CMD_LEN];
        if (n >= sizeof(cmd)) n = sizeof(cmd) - 1;
        memcpy(cmd, _rx + 2 + tl, n);
        cmd[n] = '\0';
        uint8_t r = _iot.command(cmd, now);
        _log.log(PSTR("MQTT: comando \"%.32s\"%s"), cmd, r == IoTService::OK ? "" : " recusado");
        _publishState(true, now);
    }
};
