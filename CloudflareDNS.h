// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareApi.h"
#include "WiFiService.h"

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

    // Another record: the next tick sends the IP again, whatever was synced
    void resync() {
        _syncedIP[0] = 0;
        _attempted = false;
    }

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
        char path[sizeof("/client/v4/zones//dns_records/") + sizeof(_cfg.cf_zone) + sizeof(_cfg.cf_record)];
        char payload[sizeof("{\"content\":\"\"}") + sizeof(_cfg.publicIP)];
        snprintf_P(path, sizeof(path), PSTR("/client/v4/zones/%s/dns_records/%s"), _cfg.cf_zone, _cfg.cf_record);
        snprintf_P(payload, sizeof(payload), PSTR("{\"content\":\"%s\"}"), ip);
        cfapi::Answer a;
        char why[64];
        if (!cfapi::call(_cfg, _clock, "PATCH", path, payload, a, why)) {
            _log.log(PSTR("DNS: %s"), why);
            return;
        }
        strlcpy(_syncedIP, ip, sizeof(_syncedIP));
        _cfg.lastDnsUpdate = millis();
        _wifi.notifySuccess();
        _log.log(PSTR("DNS updated!"));
    }
};
