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

// ntfy.sh both ways.
//
// Publishing (data topic): the public IP at boot and when it changes, plus
// the IoT readings and changes ([sensor], [actuator]). The IP comes from
// PublicIPResolver (Config): it does not depend on Cloudflare. The
// certificate is validated here too: the post carries the token and the
// buttons carry the command topic, which a man in the middle could use to
// drive the equipment.
//
// Commands (command topic, optional): a continuous subscription
// (GET /<topic>/json) delivers each message right away, and it becomes an
// actuator command, with the same text as the panel and the API. The ESP-01
// only has memory for one TLS session: before a post, or when DNS or OTA
// will use the network (hold), the stream closes; on reopening it asks for
// since=<last id>, and ntfy (which keeps messages for 12 h) delivers what
// arrived meanwhile. The ntfy.sh certificate is validated: a command forged
// on the way would turn the equipment on.
class NtfyNotifier {
public:
    NtfyNotifier(Config& cfg, Logger& log, Clock& clock, IoTService* iot = nullptr)
        : _cfg(cfg), _log(log), _iot(iot), _clock(clock),
          _lastAttempt(0), _attempted(false) {
        _lastSent[0] = '\0';
        _lastId[0] = '\0';
        _reply[0] = '\0';
    }

    ~NtfyNotifier() { _closeStream(); }

    void tick(unsigned long now) {
        if (_postDue(now)) {
            _closeStream();   // one TLS session at a time
            _publish(now);
            return;
        }
        _tickStream(now);
    }

    // DNS or OTA will use TLS: the stream closes and frees the heap
    void hold(bool on) {
        _held = on;
        if (on) _closeStream();
    }

    // Command stream state, for the panel
    const char* commandStatus() const {
        if (!_wantStream()) return "off";
        if (_state == ST_BODY) return "connected";
        return _held ? "paused" : "connecting";
    }

    // ntfy topics: 1–64 characters [A-Za-z0-9_-]
    static bool isValidTopic(const String& topic) {
        if (topic.length() == 0 || topic.length() >= sizeof(Config::ntfy_topic)) return false;
        for (size_t i = 0; i < topic.length(); i++) {
            char c = topic.c_str()[i];
            if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
        }
        return true;
    }

    static constexpr unsigned long RETRY_INTERVAL = 60000UL;  // IP and periodic readings
    static constexpr unsigned long POST_GAP       = 5000UL;   // events (ntfy.sh rate limit)
    static constexpr unsigned long CONNECT_MS     = 5000UL;
    static constexpr unsigned long IDLE_TIMEOUT   = 120000UL; // ntfy keepalive: 45 s
    static constexpr unsigned long RECONNECT_MS   = 2000UL;
    static constexpr uint16_t      BACKOFF_MAX_S  = 300;
    static constexpr long          STALE_S        = 600;      // an old command does not count
    static constexpr size_t        LINE_LEN       = 256;

private:
    Config&           _cfg;
    Logger&           _log;
    IoTService*       _iot;
    Clock&            _clock;

    char          _lastSent[16];
    unsigned long _lastAttempt;
    bool          _attempted;
    char          _reply[IoTService::MSG_LEN];

    // Command stream
    enum : uint8_t { ST_IDLE, ST_STATUS, ST_HEADERS, ST_BODY };
    TlsClient          _tls;
    BearSSL::X509List* _trust = nullptr;
    uint8_t       _state = ST_IDLE;
    bool          _held = false;
    bool          _overflow = false;
    bool          _waiting = false;
    size_t        _len = 0;
    char          _line[LINE_LEN] = {};
    char          _lastId[24];
    unsigned long _nextConnect = 0;
    unsigned long _lastData = 0;
    uint16_t      _backoff = 5;

    // ── Envio ────────────────────────────────────────────────────────────────

    // An event (command reply, state change) goes out in seconds; the IP and
    // periodic readings, every minute
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

        // IP from PublicIPResolver: goes to the topic at boot and when it changes
        const char* ip = _cfg.publicIP;
        if (!ip[0] || strcmp(ip, _lastSent) == 0) return;

        char body[24];
        snprintf(body, sizeof(body), "[ip] %s", ip);
        if (_post(body))
            strlcpy(_lastSent, ip, sizeof(_lastSent));
    }

    bool _post(const char* body, bool actions = false) {
        if (!PublicIPResolver::memoryOk()) {
            _log.log(PSTR("ntfy: not enough memory"));
            return false;
        }
        if (!_clock.synced()) {
            _log.log(PSTR("ntfy: clock not synchronized"));
            return false;
        }
        BearSSL::X509List trust;
        cloudflareRoots(trust);  // inclui ISRG Root X1
        TlsClient client;
        client.setTrustAnchors(&trust);
        client.setX509Time(_clock.now());
        if (!client.prepare(5000)) {
            _log.log(PSTR("ntfy: TLS unavailable"));
            return false;
        }
        HTTPClient http;
        http.setTimeout(5000);

        String url = "https://ntfy.sh/";
        url += _cfg.ntfy_topic;

        if (!http.begin(client, url)) {
            _log.log(PSTR("ntfy: connection failed"));
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
            _log.log(PSTR("ntfy sent (%d)"), code);
            return true;
        }
        _log.log(PSTR("ntfy error: %d"), code);
        return false;
    }

    // Buttons on the phone notification: each one publishes a command to the
    // command topic (with the token, if any). At most three, by what the
    // actuator does; with the rule, the third goes back to automatic.
    void _actions(HTTPClient& http) {
        if (!_cfg.ntfy_cmd[0] || !_iot->hasActuator()) return;
        const iot::ActuatorDef* a = _iot->actuatorDef();
        char pulse[12];
        snprintf(pulse, sizeof(pulse), "pulse %u", a->pulse);
        const char* labels[3];
        const char* bodies[3];
        switch (a->cap) {
            case iot::C_LEVEL:
                labels[0] = "Off";   bodies[0] = "off";
                labels[1] = "50%";   bodies[1] = "50%";
                labels[2] = "100%";  bodies[2] = "100%";
                break;
            case iot::C_POSITION:
                labels[0] = "Open";  bodies[0] = "open";
                labels[1] = "Close"; bodies[1] = "close";
                labels[2] = "Pulse"; bodies[2] = pulse;
                break;
            default: // on/off and color
                labels[0] = "On";    bodies[0] = "on";
                labels[1] = "Off";   bodies[1] = "off";
                labels[2] = "Pulse"; bodies[2] = pulse;
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

    // ── Commands ─────────────────────────────────────────────────────────────

    bool _wantStream() const {
        return _iot && _iot->hasActuator() && _cfg.ntfy_cmd[0];
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
        // Reads what arrived without waiting: at most one full line per tick
        for (size_t i = 0; i < LINE_LEN && _tls.available() > 0; i++) {
            _lastData = now;
            _feed((char)_tls.read(), now);
            if (_state == ST_IDLE) return;
        }
        if (!_tls.connected()) {
            _log.log(PSTR("ntfy: command stream closed"));
            _drop(now, RECONNECT_MS);
            return;
        }
        if (now - _lastData > IDLE_TIMEOUT) {
            _log.log(PSTR("ntfy: command stream stalled"));
            _drop(now, RECONNECT_MS);
        }
    }

    void _connect(unsigned long now) {
        if (!_clock.synced()) {
            _retry(now, RECONNECT_MS);   // the certificate needs a correct clock
            return;
        }
        if (!PublicIPResolver::memoryOk()) {
            _log.log(PSTR("ntfy: not enough memory for commands"));
            _fail(now);
            return;
        }
        _trust = new BearSSL::X509List();
        cloudflareRoots(*_trust);  // inclui ISRG Root X1
        _tls.setTrustAnchors(_trust);
        _tls.setX509Time(_clock.now());
        if (!_tls.prepare(CONNECT_MS) || !_tls.connect("ntfy.sh", 443)) {
            _log.log(PSTR("ntfy: no connection for commands"));
            _fail(now);
            return;
        }
        // HTTP/1.0: the server sends the stream directly, not chunked
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
            else _overflow = true;   // long line (attachment, title): ignored
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
                _log.log(PSTR("ntfy: commands HTTP %d"), code);
                _fail(now);
                return;
            }
            _state = ST_HEADERS;
        } else if (_state == ST_HEADERS) {
            if (_line[0] == '\0') {
                _state = ST_BODY;
                _backoff = 5;
                _log.log(PSTR("ntfy: commands connected"));
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
        long age = (long)(_clock.now() - (time_t)(doc["time"] | 0L));
        if (age > STALE_S) {
            _log.log(PSTR("ntfy: old command ignored"));
            return;
        }
        uint8_t r = _iot->command(msg, now);
        _log.log(PSTR("ntfy: command \"%.32s\""), msg);
        if (r == IoTService::OK) return;
        const char* why = r == IoTService::INVALID ? "invalid command"
                        : "command not valid for this actuator";
        snprintf(_reply, sizeof(_reply), "[error] %s: %.32s", why, msg);
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

    // Repeated failures wait longer each time (5 s up to 5 min)
    void _fail(unsigned long now) {
        _drop(now, (unsigned long)_backoff * 1000UL);
        _backoff = _backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : (uint16_t)(_backoff * 2);
    }

    void _closeStream() {
        if (_trust) _tls.stop();   // there is only a session after the roots are allocated
        delete _trust;
        _trust = nullptr;
        _state = ST_IDLE;
        _waiting = false;
    }
};
