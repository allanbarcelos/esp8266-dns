// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareCA.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"  // TlsClient

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
        String url = "https://api.cloudflare.com/client/v4/zones/";
        url += _cfg.cf_zone;
        url += "/dns_records/";
        url += record;

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

        HTTPClient http;
        http.setTimeout(5000);
        if (!http.begin(client, url)) {
            _log.log(PSTR("DNS: connection failed"));
            return false;
        }
        http.addHeader("Authorization", String("Bearer ") + _cfg.cf_token);
        http.addHeader("Content-Type", "application/json");

        String payload = "{\"content\":\"";
        payload += ip;
        payload += "\"}";

        int code = http.PATCH(payload);
        String resp = (code > 0) ? http.getString() : String();
        http.end();
        client.stop();  // HTTPClient keep-alive would keep the TLS session open

        if (code <= 0) {
            _log.log(PSTR("DNS connection error: %d"), code);
            return false;
        }

        // Parsed in place (char*), like the ntfy messages: a single JSON
        // reader in the firmware, and the strings stay in the response. The
        // document only holds the structure, one slot per value, and TLS is
        // already closed: the heap is free.
        DynamicJsonDocument doc(JSON_ARRAY_SIZE(_values(resp)));
        DeserializationError err = deserializeJson(doc, resp.begin());
        if (err || doc["success"].isNull()) {
            _log.log(PSTR("DNS invalid response (HTTP %d)"), code);
            return false;
        }

        if (!(doc["success"] | false)) {
            int         ec  = doc["errors"][0]["code"]    | 0;
            const char* msg = doc["errors"][0]["message"] | "Unknown error";
            _log.log(PSTR("DNS error %d: %s"), ec, msg);
            return false;
        }
        return true;
    }

    // Upper bound on the values in the JSON: each value after the first in
    // an object or list follows a comma, and the first follows { or [. A
    // comma inside text only raises the count.
    static size_t _values(const String& json) {
        size_t n = 0;
        for (const char* p = json.c_str(); *p; p++)
            n += *p == ',' || *p == '{' || *p == '[';
        return n;
    }
};
