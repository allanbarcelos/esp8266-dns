// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <algorithm>
#include <bearssl/bearssl.h>
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareCA.h"
#include "HttpServer.h"
#include "IoTService.h"
#include "NtfyNotifier.h"
#include "PublicIPResolver.h"  // memoryOk
#include "RelayScript.h"
#include "TlsClient.h"
#include "WsClient.h"

// Remote access without an open port: the device opens a WebSocket to its
// own Cloudflare Worker (relay/worker.mjs, deployed by CloudflareSetup.h)
// and keeps it. The Worker sends each request from the internet down this
// connection; the device answers it with the same HttpServer, routes and
// sign-in as on the local network, and the answer goes back up.
//
// Frames (binary, first byte = type; id = 16 bits, big endian):
//   'R' id ip[4] request    Worker → device: raw HTTP request, client IPv4
//   'D' id bytes            device → Worker: a piece of the answer
//   'E' id                  device → Worker: the answer is complete
//   'H' json                device → Worker: hello, after connecting
//   'S' json                device → Worker: the state of /api/state
//   'C' json                Worker → device: a message of the ntfy command topic
// plus the text "ping" every PING_MS (the Worker answers "pong" without
// waking up). The Worker checks the shared secret on connect; a protocol
// version it does not speak gets 426, and the device redeploys it.
//
// One TLS session at a time fits in RAM: hold() closes the connection
// while DNS, OTA or an ntfy post need theirs, and the Worker waits for the
// device to come back before giving up on a request. For the same reason
// the Worker polls the ntfy command topic for the device (the hello says
// which, with the token, the interval and the last message seen); if the
// relay stays down longer than GRACE_MS, the ntfy stream takes over.
class RelayService {
public:
    static constexpr uint8_t       PROTOCOL      = 1;
    static constexpr unsigned long CONNECT_MS    = 5000UL;
    static constexpr unsigned long RECONNECT_MS  = 2000UL;
    static constexpr unsigned long PING_MS       = 30000UL;
    static constexpr unsigned long IDLE_MS       = 90000UL;   // nothing from the Worker: gone
    static constexpr unsigned long STATE_MS      = 1000UL;    // a changed state goes up within 1 s
    static constexpr unsigned long TELE_MS       = 300000UL;  // and every 5 min even without a change
    static constexpr uint16_t      BACKOFF_MAX_S = 300;
    static constexpr int           FRAMES_PER_TICK = 4;
    static constexpr unsigned long GRACE_MS      = 60000UL;   // down this long: ntfy on its own

    RelayService(Config& cfg, Logger& log, Clock& clock, HttpServer& http, IoTService* iot = nullptr,
                 NtfyNotifier* ntfy = nullptr)
        : _cfg(cfg), _log(log), _clock(clock), _http(http), _iot(iot), _ntfy(ntfy) {}

    ~RelayService() { _close(); }

    void tick(unsigned long now) {
        if (_held || !configured() || WiFi.status() != WL_CONNECTED) {
            _close();
            return;
        }
        if (!_open) {
            if ((long)(now - _next) >= 0) _connect(now);
            return;
        }
        _pump(now);
    }

    // Another TLS session is about to be used: the connection closes
    void hold(bool on) {
        _held = on;
        if (on) _close();
    }

    // Settings changed: connect again now, with them
    void reconnect() {
        _close();
        _next = millis();
        _backoff = 5;
        _outdated = false;
    }

    // Deployed for this protocol version, with a secret
    bool configured() const {
        return _cfg.relay_host[0] && _cfg.relay_secret[0] && _cfg.relay_ver == PROTOCOL;
    }

    bool up() const       { return _open; }

    // Uses the network: connected, or due to connect
    bool wantsNet(unsigned long now) const {
        return configured() && WiFi.status() == WL_CONNECTED && (_open || (long)(now - _next) >= 0);
    }

    // The ntfy commands come through the relay: it is up, or was until moments ago
    bool carries(unsigned long now) const { return _open || (configured() && now - _downAt < GRACE_MS); }
    bool outdated() const { return _outdated; }   // the Worker refused this version (426)

    const char* status() const {
        if (!configured()) return "off";
        if (_open) return "connected";
        if (_outdated) return "outdated";
        return _held ? "paused" : "connecting";
    }

private:
    // The request inside an 'R' frame, as HttpServer reads it, and the
    // answer as 'D' frames
    class Link : public HttpLink {
    public:
        explicit Link(RelayService& r) : _r(r) {}
        int  available() override { return _r._s->ws.left() && _r._s->tls.available() > 0; }
        int  read() override { return _r._s->ws.next(); }
        bool connected() override { return _r._s->ws.left() && _r._s->tls.connected(); }
        void write(const uint8_t* p, size_t n) override {
            uint8_t h[3] = {'D', (uint8_t)(_r._id >> 8), (uint8_t)_r._id};
            _r._s->ws.send(WsClient::BINARY, h, sizeof(h), p, n);
        }
        uint32_t remoteIP() override { return _r._ip; }
    private:
        RelayService& _r;
    };

    Config&            _cfg;
    Logger&            _log;
    Clock&             _clock;
    HttpServer&        _http;
    IoTService*        _iot;
    NtfyNotifier*      _ntfy;
    // Only while connected: the TLS client and its roots take heap even
    // when idle, and OTA needs every byte of it
    struct Session {
        BearSSL::X509List trust;
        TlsClient         tls;
        WsClient          ws{tls};
    };
    Session*           _s = nullptr;
    Link               _link{*this};

    bool          _open = false;
    bool          _held = false;
    bool          _outdated = false;
    unsigned long _next = 0;
    unsigned long _lastIn = 0, _lastPing = 0, _lastState = 0, _stateSent = 0, _downAt = 0;
    uint16_t      _backoff = 5;
    uint16_t      _id = 0;
    uint32_t      _ip = 0;
    uint32_t      _sum = 0;      // of the last state sent
    uint32_t      _helloSum = 0; // of what the hello carried

    void _connect(unsigned long now) {
        if (!_clock.synced()) {
            _next = now + RECONNECT_MS;   // the certificate needs a correct clock
            return;
        }
        if (!PublicIPResolver::memoryOk()) {
            _log.log(PSTR("relay: not enough memory"));
            _fail(now);
            return;
        }
        _s = new Session();
        cloudflareRoots(_s->trust);
        _s->tls.setTrustAnchors(&_s->trust);
        _s->tls.setX509Time(_clock.now());
        if (!_s->tls.prepare(CONNECT_MS) || !_s->tls.connect(_cfg.relay_host, 443)) {
            _log.log(PSTR("relay: no connection"));
            _fail(now);
            return;
        }
        char extra[sizeof("Authorization: Bearer \r\nX-Relay-Version: 255\r\n") + sizeof(_cfg.relay_secret)];
        snprintf_P(extra, sizeof(extra), PSTR("Authorization: Bearer %s\r\nX-Relay-Version: %u\r\n"),
                   _cfg.relay_secret, (unsigned)PROTOCOL);
        int code = _s->ws.handshake(_cfg.relay_host, "/.relay", extra);
        if (code != 101) {
            _log.log(PSTR("relay: refused (HTTP %d)"), code);
            _outdated = code == 426;
            _fail(now);
            return;
        }
        _open = true;
        _backoff = 5;
        _lastIn = _lastPing = _lastState = now;
        _log.log(PSTR("relay: connected"));
        _hello();
        _sendState(true, now);
    }

    void _pump(unsigned long now) {
        // A few frames per tick: the rest of the loop goes on
        for (int i = 0; i < FRAMES_PER_TICK && _s->ws.frame(); i++) {
            _lastIn = now;
            _frame();
            if (!_open) {
                _next = now + RECONNECT_MS;
                return;
            }
        }
        if (_s->ws.error() || !_s->tls.connected()) {
            _log.log(PSTR("relay: connection lost"));
            _drop(now, RECONNECT_MS);
            return;
        }
        if (now - _lastIn >= IDLE_MS) {
            _log.log(PSTR("relay: stalled"));
            _drop(now, RECONNECT_MS);
            return;
        }
        if (now - _lastPing >= PING_MS) {
            _lastPing = now;
            _s->ws.send(WsClient::TEXT, "ping", 4);
        }
        if (now - _lastState >= STATE_MS) {
            _lastState = now;
            if (_settingsSum() != _helloSum) _hello();   // a new API token or ntfy settings
            _sendState(now - _stateSent >= TELE_MS, now);
        }
    }

    void _frame() {
        uint8_t op = _s->ws.op();
        if (op == WsClient::PING) {
            char b[126];
            size_t n = _s->ws.read(b, sizeof(b));
            _s->ws.send(WsClient::PONG, b, n);
        } else if (op == WsClient::CLOSE) {
            _s->ws.skip();
            _log.log(PSTR("relay: closed by the Worker"));
            _close();
        } else if (op != WsClient::BINARY) {
            _s->ws.skip();
        } else if (int type = _s->ws.next(); type == 'R') {
            _request();
        } else if (type != 'C' || !_ntfy) {
            _s->ws.skip();
        } else {
            // An ntfy message; one longer than a stream line is no command
            char line[NtfyNotifier::LINE_LEN];
            bool whole = _s->ws.left() < sizeof(line);
            _s->ws.read(line, sizeof(line));
            if (whole) _ntfy->deliver(line, millis());
        }
    }

    // id, client address, then the request itself through HttpServer
    void _request() {
        uint8_t h[6];
        for (uint8_t& b : h) {
            int c = _s->ws.next();
            if (c < 0) return;
            b = (uint8_t)c;
        }
        _id = (uint16_t)(h[0] << 8 | h[1]);
        _ip = (uint32_t)h[2] | (uint32_t)h[3] << 8 | (uint32_t)h[4] << 16 | (uint32_t)h[5] << 24;
        _http.serve(_link);
        _s->ws.skip();   // what the request had beyond its Content-Length
        uint8_t end[3] = {'E', h[0], h[1]};
        _s->ws.send(WsClient::BINARY, end, sizeof(end));
    }

    // Version, firmware and a keyed hash of the API token: with the device
    // offline, the Worker answers /api/state from the last state to whoever
    // holds that token, without ever storing the token itself
    void _hello() {
        char mac[41] = "";
        if (_cfg.api_token[0]) {
            uint8_t d[20];
            br_hmac_key_context kc;
            br_hmac_context hc;
            br_hmac_key_init(&kc, &br_sha1_vtable, _cfg.relay_secret, strlen(_cfg.relay_secret));
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, _cfg.api_token, strlen(_cfg.api_token));
            br_hmac_out(&hc, d);
            for (int i = 0; i < 20; i++) snprintf(mac + 2 * i, 3, "%02x", d[i]);
        }
        // The ntfy commands: topic, token, interval and the last message seen
        // (the IDs are letters and digits, the rest is checked on the panel)
        char b[320];
        int n = snprintf_P(b, sizeof(b), PSTR("H{\"v\":%u,\"fw\":\"%s\",\"api\":\"%s\""),
                           (unsigned)PROTOCOL, firmware_version, mac);
        if (_ntfy && _ntfy->wantsCommands()) {
            const char* since = _ntfy->lastId();
            n += snprintf_P(b + n, sizeof(b) - n, PSTR(",\"topic\":\"%s\",\"token\":\"%s\",\"poll\":%u,\"since\":\"%s\""),
                            _cfg.ntfy_cmd, _cfg.ntfy_token, (unsigned)_cfg.ntfy_poll,
                            strspn(since, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == strlen(since)
                                ? since : "");
        }
        b[n++] = '}';
        _s->ws.send(WsClient::BINARY, b, (size_t)n);
        _helloSum = _settingsSum();
    }

    // What the hello carries that can change while connected
    uint32_t _settingsSum() const {
        bool cmd = _ntfy && _ntfy->wantsCommands();
        return _fnv(_cfg.api_token, strlen(_cfg.api_token)) ^
               (cmd ? _fnv(_cfg.ntfy_cmd, strlen(_cfg.ntfy_cmd)) * 3 + _fnv(_cfg.ntfy_token, strlen(_cfg.ntfy_token)) +
                          _cfg.ntfy_poll
                    : 0);
    }

    static uint32_t _fnv(const char* p, size_t n) {
        uint32_t sum = 2166136261UL;
        for (size_t i = 0; i < n; i++) sum = (sum ^ (uint8_t)p[i]) * 16777619UL;
        return sum;
    }

    void _sendState(bool force, unsigned long now) {
        if (!_iot || (!_iot->hasSensor() && !_iot->hasActuator())) return;
        char json[1 + 384];
        json[0] = 'S';
        size_t n = std::min(_iot->stateJson(json + 1, sizeof(json) - 1), sizeof(json) - 2);
        uint32_t sum = _fnv(json + 1, n);
        if (!force && sum == _sum) return;
        _sum = sum;
        _stateSent = now;
        _s->ws.send(WsClient::BINARY, json, n + 1);
    }

    void _drop(unsigned long now, unsigned long ms) {
        _close();
        _next = now + ms;
    }

    // Repeated failures wait longer each time (5 s up to 5 min)
    void _fail(unsigned long now) {
        _drop(now, (unsigned long)_backoff * 1000UL);
        _backoff = _backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : (uint16_t)(_backoff * 2);
    }

    void _close() {
        if (_open) _downAt = millis();
        if (_s) _s->tls.stop();
        delete _s;
        _s = nullptr;
        _open = false;
    }
};

// The Worker in the firmware speaks this protocol (tools/embed_relay.py checks it too)
static_assert(RELAY_VERSION == RelayService::PROTOCOL, "relay/worker.mjs VERSION and RelayService::PROTOCOL differ");
