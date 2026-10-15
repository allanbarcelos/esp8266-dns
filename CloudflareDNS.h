// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include "Json.h"
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareCA.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"  // memoryOk
#include "TlsClient.h"

// Keeps the Cloudflare A record pointing at the public IP.
//
// The IP comes from PublicIPResolver (Config::publicIP). The one already
// synced is kept in memory: we do not rely on the local DNS resolver (which
// caches by TTL and returns Cloudflare IPs for proxied records). After a
// boot, the first known IP always sends a PATCH (idempotent); a failure
// retries every RETRY_INTERVAL.
class CloudflareDNS {
public:
    CloudflareDNS(Config& cfg, Logger& log, WiFiService& wifi, Clock& clock)
        : _cfg(cfg), _log(log), _wifi(wifi), _clock(clock) {
        _syncedIP[0] = '\0';
    }

    void tick(unsigned long now) {
        if (!due(now)) return;
        _attempted   = true;
        _lastAttempt = now;
        _sync();
    }

    // The next tick call will use the network (TLS)
    bool due(unsigned long now) const {
        return configured() && _cfg.publicIP[0] && strcmp(_cfg.publicIP, _syncedIP) != 0 &&
               (!_attempted || now - _lastAttempt >= RETRY_INTERVAL) &&
               _wifi.isConnected() && _clock.synced();  // TLS needs a valid clock
    }

    bool configured() const {
        return _cfg.cf_token[0] && _cfg.cf_zone[0] && _cfg.cf_record[0];
    }

    const char* syncedIP() const { return _syncedIP; }

    static constexpr unsigned long RETRY_INTERVAL = 60000UL;

private:
    Config&       _cfg;
    Logger&       _log;
    WiFiService&  _wifi;
    Clock&        _clock;
    unsigned long _lastAttempt = 0;
    bool          _attempted   = false;
    char          _syncedIP[16];

    void _sync() {
        char ip[16];
        strlcpy(ip, _cfg.publicIP, sizeof(ip));
        _log.log(PSTR("Updating DNS to %s"), ip);
        BearSSL::X509List trust;
        cloudflareRoots(trust);
        if (_patchRecord(trust, _cfg.cf_record, ip)) {
            strlcpy(_syncedIP, ip, sizeof(_syncedIP));
            _cfg.lastDnsUpdate = millis();
            _wifi.notifySuccess();
            _log.log(PSTR("DNS updated!"));
        }
    }

    bool _patchRecord(const BearSSL::X509List& trust, const char* record, const char* ip) {
        if (!PublicIPResolver::memoryOk()) {
            _log.log(PSTR("DNS: not enough memory"));
            return false;
        }
        TlsClient client;
        client.setTrustAnchors(&trust);
        client.setX509Time(_clock.now());
        if (!client.prepare(5000)) {
            _log.log(PSTR("DNS: TLS unavailable"));
            return false;
        }

        char path[sizeof("/client/v4/zones//dns_records/") + sizeof(_cfg.cf_zone) + sizeof(_cfg.cf_record)];
        char auth[sizeof("Authorization: Bearer \r\nContent-Type: application/json\r\n") + sizeof(_cfg.cf_token)];
        char payload[sizeof("{\"content\":\"\"}") + sizeof(_cfg.publicIP)];
        snprintf_P(path, sizeof(path), PSTR("/client/v4/zones/%s/dns_records/%s"), _cfg.cf_zone, record);
        snprintf_P(auth, sizeof(auth), PSTR("Authorization: Bearer %s\r\nContent-Type: application/json\r\n"),
                   _cfg.cf_token);
        snprintf_P(payload, sizeof(payload), PSTR("{\"content\":\"%s\"}"), ip);
        String resp;
        int code = https::request(client, "api.cloudflare.com", "PATCH", path, auth, payload, 5000, &resp);

        if (code <= 0) {
            _log.log(PSTR("DNS connection error: %d"), code);
            return false;
        }

        // TLS is already closed: the heap is free for the answer
        Answer a;
        if (!json::object(resp.c_str(), _answer, &a) || a.success < 0) {
            _log.log(PSTR("DNS invalid response (HTTP %d)"), code);
            return false;
        }
        if (!a.success) {
            _log.log(PSTR("DNS error %d: %s"), (int)a.code, a.message);
            return false;
        }
        return true;
    }

    // What matters in the API answer: "success" (-1 absent or null, 1 only
    // for true) and, on an error, the code and message of errors[0]
    struct Answer {
        int8_t  success = -1;
        int32_t code    = 0;
        char    message[64] = "Unknown error";
    };

    static void _answer(void* ctx, const char* key, size_t len, const json::Value& v) {
        Answer& a = *static_cast<Answer*>(ctx);
        if (json::is(key, len, PSTR("success"))) {
            a.success = v.kind == json::NUL ? -1 : v.kind == json::BOOL && v.mag;
        } else if (json::is(key, len, PSTR("errors")) && v.kind == json::ARRAY) {
            json::Value e = json::first(v);
            if (e.kind == json::OBJECT) json::members(e, _error, &a);
        }
    }

    static void _error(void* ctx, const char* key, size_t len, const json::Value& v) {
        Answer& a = *static_cast<Answer*>(ctx);
        uint32_t n;
        if (json::is(key, len, PSTR("code")) && v.fits(-2147483647L, 0x7FFFFFFFu, n)) a.code = (int32_t)n;
        if (json::is(key, len, PSTR("message")) && v.kind == json::STRING)
            json::copy(v, a.message, sizeof(a.message));
    }
};
