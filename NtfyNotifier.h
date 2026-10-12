// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "PublicIPResolver.h"
#include "IoTService.h"

// ntfy.sh nos dois sentidos.
//
// Envio (tópico de dados): o IP público no boot e quando muda, e as
// leituras e mudanças do IoT ([sensor], [atuador]). Descobre o IP sozinho:
// não depende do Cloudflare.
//
// Comandos (tópico de comandos, opcional): uma assinatura contínua
// (GET /<tópico>/json) entrega cada mensagem na hora, e ela vira um
// comando do atuador, com o mesmo texto do painel e da API. O ESP-01 só
// tem memória para uma sessão TLS: antes de um envio, ou quando o DNS ou
// o OTA vão usar a rede (hold), o fluxo fecha; ao reabrir pede
// since=<último id>, e o ntfy (que guarda as mensagens por 12 h) entrega
// o que chegou no intervalo. O certificado do ntfy.sh é validado: um
// comando forjado no caminho ligaria o equipamento.
class NtfyNotifier {
public:
    NtfyNotifier(Config& cfg, Logger& log, PublicIPResolver& resolver,
                 IoTService* iot = nullptr, Clock* clock = nullptr)
        : _cfg(cfg), _log(log), _resolver(resolver), _iot(iot), _clock(clock),
          _lastAttempt(0), _attempted(false) {
        _lastSent[0] = '\0';
        _lastId[0] = '\0';
        _reply[0] = '\0';
    }

    ~NtfyNotifier() { _closeStream(); }

    void tick(unsigned long now) {
        if (_postDue(now)) {
            _closeStream();   // uma sessão TLS por vez
            _publish(now);
            return;
        }
        _tickStream(now);
    }

    // DNS ou OTA vão usar TLS: o fluxo fecha e libera a heap
    void hold(bool on) {
        _held = on;
        if (on) _closeStream();
    }

    // Situação dos comandos, para o painel
    const char* commandStatus() const {
        if (!_wantStream()) return "desligado";
        if (_state == ST_BODY) return "conectado";
        return _held ? "pausado" : "conectando";
    }

    // Tópicos ntfy: 1–64 caracteres [A-Za-z0-9_-]
    static bool isValidTopic(const String& topic) {
        if (topic.length() == 0 || topic.length() >= sizeof(Config::ntfy_topic)) return false;
        for (size_t i = 0; i < topic.length(); i++) {
            char c = topic.c_str()[i];
            if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
        }
        return true;
    }

    static constexpr unsigned long RETRY_INTERVAL = 60000UL;  // IP e leituras periódicas
    static constexpr unsigned long POST_GAP       = 5000UL;   // eventos (limite do ntfy.sh)
    static constexpr unsigned long CONNECT_MS     = 5000UL;
    static constexpr unsigned long IDLE_TIMEOUT   = 120000UL; // keepalive do ntfy: 45 s
    static constexpr unsigned long RECONNECT_MS   = 2000UL;
    static constexpr uint16_t      BACKOFF_MAX_S  = 300;
    static constexpr long          STALE_S        = 600;      // comando velho não vale
    static constexpr size_t        LINE_LEN       = 256;

private:
    Config&           _cfg;
    Logger&           _log;
    PublicIPResolver& _resolver;
    IoTService*       _iot;
    Clock*            _clock;

    char          _lastSent[16];
    unsigned long _lastAttempt;
    bool          _attempted;
    char          _reply[IoTService::MSG_LEN];

    // Fluxo de comandos
    enum : uint8_t { ST_IDLE, ST_STATUS, ST_HEADERS, ST_BODY };
    TlsClient          _tls;
    BearSSL::X509List* _trust = nullptr;
    uint8_t       _state = ST_IDLE;
    bool          _held = false;
    bool          _overflow = false;
    bool          _waiting = false;
    size_t        _len = 0;
    char          _line[LINE_LEN];
    char          _lastId[24];
    unsigned long _nextConnect = 0;
    unsigned long _lastData = 0;
    uint16_t      _backoff = 5;

    // ── Envio ────────────────────────────────────────────────────────────────

    // Evento (resposta a comando, mudança de estado) sai em segundos; IP e
    // leitura periódica, a cada minuto
    bool _postDue(unsigned long now) const {
        if (_cfg.ntfy_topic[0] == '\0') return false;
        if (!_attempted) return true;
        unsigned long gap = now - _lastAttempt;
        bool urgent = _reply[0] || (_iot && _iot->pending() && _iot->urgent());
        if (urgent && gap >= POST_GAP) return true;
        return gap >= RETRY_INTERVAL;
    }

    void _publish(unsigned long now) {
        _attempted   = true;
        _lastAttempt = now;

        if (_reply[0]) {
            if (_post(_reply)) _reply[0] = '\0';
            return;
        }
        if (_iot && _iot->pending()) {
            if (_post(_iot->message(), true)) _iot->ack();
            return;
        }

        char ip[16];
        if (!_resolver.resolve(ip, sizeof(ip), now)) return;

        strlcpy(_cfg.publicIP, ip, sizeof(_cfg.publicIP));
        if (strcmp(ip, _lastSent) == 0) return;

        char body[24];
        snprintf(body, sizeof(body), "[ip] %s", ip);
        if (_post(body))
            strlcpy(_lastSent, ip, sizeof(_lastSent));
    }

    bool _post(const char* body, bool actions = false) {
        if (!PublicIPResolver::memoryOk()) {
            _log.log(PSTR("ntfy: memoria insuficiente"));
            return false;
        }
        TlsClient client;
        client.setInsecure(); // o conteúdo (IP público, leitura) não é sigiloso
        if (!client.prepare(5000)) {
            _log.log(PSTR("ntfy: TLS indisponivel"));
            return false;
        }
        HTTPClient http;
        http.setTimeout(5000);

        String url = "https://ntfy.sh/";
        url += _cfg.ntfy_topic;

        if (!http.begin(client, url)) {
            _log.log(PSTR("ntfy: falha ao conectar"));
            return false;
        }

        http.addHeader("Content-Type", "text/plain");
        if (_cfg.ntfy_token[0]) {
            String auth = "Bearer ";
            auth += _cfg.ntfy_token;
            http.addHeader("Authorization", auth);
        }
        if (actions) _actions(http);
        int code = http.POST(body);
        http.end();

        if (code >= 200 && code < 300) {
            _log.log(PSTR("ntfy enviado (%d)"), code);
            return true;
        }
        _log.log(PSTR("ntfy erro: %d"), code);
        return false;
    }

    // Botões na notificação do celular: cada um publica um comando no tópico
    // de comandos (com o token, se houver). No máximo três, pelo que o
    // atuador faz; com a regra, o terceiro volta ao automático.
    void _actions(HTTPClient& http) {
        if (!_cfg.ntfy_cmd[0] || !_iot->hasActuator()) return;
        const iot::ActuatorDef* a = _iot->actuatorDef();
        char pulse[12];
        snprintf(pulse, sizeof(pulse), "pulso %u", a->pulse);
        const char* labels[3];
        const char* bodies[3];
        switch (a->cap) {
            case iot::C_LEVEL:
                labels[0] = "Desligar"; bodies[0] = "desligar";
                labels[1] = "50%";      bodies[1] = "50%";
                labels[2] = "100%";     bodies[2] = "100%";
                break;
            case iot::C_POSITION:
                labels[0] = "Abrir";    bodies[0] = "abrir";
                labels[1] = "Fechar";   bodies[1] = "fechar";
                labels[2] = "Pulso";    bodies[2] = pulse;
                break;
            default: // liga/desliga e cor
                labels[0] = "Ligar";    bodies[0] = "ligar";
                labels[1] = "Desligar"; bodies[1] = "desligar";
                labels[2] = "Pulso";    bodies[2] = pulse;
                break;
        }
        if (_iot->hasRule()) {
            labels[2] = "Auto";
            bodies[2] = "auto";
        }
        String out;
        for (int i = 0; i < 3; i++) {
            if (i) out += "; ";
            out += "http, ";
            out += labels[i];
            out += ", https://ntfy.sh/";
            out += _cfg.ntfy_cmd;
            out += ", method=POST, body=";
            out += bodies[i];
            out += ", clear=true";
            if (_cfg.ntfy_token[0]) {
                out += ", headers.Authorization=Bearer ";
                out += _cfg.ntfy_token;
            }
        }
        http.addHeader("Actions", out);
    }

    // ── Comandos ─────────────────────────────────────────────────────────────

    bool _wantStream() const {
        return _clock && _iot && _iot->hasActuator() && _cfg.ntfy_cmd[0];
    }

    void _tickStream(unsigned long now) {
        if (_held || !_wantStream() || WiFi.status() != WL_CONNECTED) {
            _closeStream();
            return;
        }
        if (_state == ST_IDLE) {
            if (_waiting && (long)(now - _nextConnect) < 0) return;
            _connect(now);
            return;
        }
        // Lê o que chegou sem esperar: no máximo uma linha cheia por tick
        for (size_t i = 0; i < LINE_LEN && _tls.available() > 0; i++) {
            _lastData = now;
            _feed((char)_tls.read(), now);
            if (_state == ST_IDLE) return;
        }
        if (!_tls.connected()) {
            _log.log(PSTR("ntfy: fluxo de comandos encerrado"));
            _drop(now, RECONNECT_MS);
            return;
        }
        if (now - _lastData > IDLE_TIMEOUT) {
            _log.log(PSTR("ntfy: fluxo de comandos parado"));
            _drop(now, RECONNECT_MS);
        }
    }

    void _connect(unsigned long now) {
        if (!_clock->synced()) {
            _retry(now, RECONNECT_MS);   // o certificado pede relógio certo
            return;
        }
        if (!PublicIPResolver::memoryOk()) {
            _log.log(PSTR("ntfy: memoria insuficiente para comandos"));
            _fail(now);
            return;
        }
        _trust = new BearSSL::X509List(CLOUDFLARE_CA);  // inclui ISRG Root X1
        _tls.setTrustAnchors(_trust);
        _tls.setX509Time(_clock->now());
        if (!_tls.prepare(CONNECT_MS) || !_tls.connect("ntfy.sh", 443)) {
            _log.log(PSTR("ntfy: sem conexao para comandos"));
            _fail(now);
            return;
        }
        // HTTP/1.0: o servidor manda o fluxo direto, sem chunked
        char req[200];
        snprintf(req, sizeof(req), "GET /%s/json%s%s HTTP/1.0\r\nHost: ntfy.sh\r\n"
                 "User-Agent: ESP8266\r\n", _cfg.ntfy_cmd,
                 _lastId[0] ? "?since=" : "", _lastId);
        _tls.print(req);
        if (_cfg.ntfy_token[0]) {
            snprintf(req, sizeof(req), "Authorization: Bearer %s\r\n", _cfg.ntfy_token);
            _tls.print(req);
        }
        _tls.print("\r\n");
        _state = ST_STATUS;
        _len = 0;
        _overflow = false;
        _lastData = now;
    }

    void _feed(char c, unsigned long now) {
        if (c == '\r') return;
        if (c != '\n') {
            if (_len < LINE_LEN - 1) _line[_len++] = c;
            else _overflow = true;   // linha longa (anexo, título): ignorada
            return;
        }
        _line[_len] = '\0';
        bool skip = _overflow;
        _len = 0;
        _overflow = false;
        if (skip) return;

        if (_state == ST_STATUS) {
            const char* sp = strchr(_line, ' ');
            int code = sp ? atoi(sp + 1) : 0;
            if (code != 200) {
                _log.log(PSTR("ntfy: comandos HTTP %d"), code);
                _fail(now);
                return;
            }
            _state = ST_HEADERS;
        } else if (_state == ST_HEADERS) {
            if (_line[0] == '\0') {
                _state = ST_BODY;
                _backoff = 5;
                _log.log(PSTR("ntfy: comandos conectados"));
            }
        } else {
            _onEvent(now);
        }
    }

    void _onEvent(unsigned long now) {
        StaticJsonDocument<JSON_OBJECT_SIZE(12)> doc;
        if (deserializeJson(doc, _line)) return;    // lixo
        const char* event = doc["event"] | "";
        if (strcmp(event, "message") != 0) return;   // open, keepalive
        strlcpy(_lastId, doc["id"] | "", sizeof(_lastId));
        const char* msg = doc["message"] | "";
        long age = (long)(_clock->now() - (time_t)(doc["time"] | 0L));
        if (age > STALE_S) {
            _log.log(PSTR("ntfy: comando antigo ignorado"));
            return;
        }
        uint8_t r = _iot->command(msg, now);
        _log.log(PSTR("ntfy: comando \"%.32s\""), msg);
        if (r == IoTService::OK) return;
        const char* why = r == IoTService::INVALID ? "comando invalido"
                        : "comando nao vale para este atuador";
        snprintf(_reply, sizeof(_reply), "[erro] %s: %.32s", why, msg);
        _log.log(PSTR("ntfy: %s"), _reply);
    }

    void _retry(unsigned long now, unsigned long ms) {
        _waiting = true;
        _nextConnect = now + ms;
    }

    void _drop(unsigned long now, unsigned long ms) {
        _closeStream();
        _retry(now, ms);
    }

    // Falha seguida espera cada vez mais (5 s até 5 min)
    void _fail(unsigned long now) {
        _drop(now, (unsigned long)_backoff * 1000UL);
        _backoff = _backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : (uint16_t)(_backoff * 2);
    }

    void _closeStream() {
        if (_trust) _tls.stop();   // só há sessão depois de alocar as raízes
        delete _trust;
        _trust = nullptr;
        _state = ST_IDLE;
        _waiting = false;
    }
};
