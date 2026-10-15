// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareCA.h"
#include "ReverseDNS.h"
#include "TlsClient.h"

// Public IP over HTTPS. The three providers return only the IPv4 and use
// the WE1 → GTS Root R4 chain, the same one api.cloudflare.com already
// validates in the field with the roots in CloudflareCA.h. A service outside
// that chain is not used: TLS would fail on this device.
//
// Checks every CHECK_INTERVAL, with or without DNS and ntfy set up; the
// first valid IPv4 answer ends the search (one TLS session per cycle). The
// result goes to Config: DNS, ntfy and the panel read it from there. The
// log only gets changes: a new IP, or starting to fail.
//
// Host and path of each service, in order (in RAM: they go to print())
static const char* const IP_SERVICES[][2] = {
    {"api4.ipify.org", "/"}, {"icanhazip.com", "/"}, {"ifconfig.co", "/ip"},
};
static const uint8_t NUM_IP_SERVICES = 3;

class PublicIPResolver {
public:
    PublicIPResolver(Config& cfg, Logger& log, Clock& clock)
        : _cfg(cfg), _log(log), _clock(clock) {}

    void tick(unsigned long now) {
        _rdns.tick(now);
        if (!due(now)) return;
        _checked   = true;
        _lastCheck = now;
        _check(now);
    }

    // The next tick call will use the network (TLS)
    bool due(unsigned long now) const {
        return WiFi.isConnected() && _clock.synced() &&
               (!_checked || now - _lastCheck >= (_failing ? RETRY_INTERVAL : CHECK_INTERVAL));
    }

    static constexpr unsigned long CHECK_INTERVAL  = 300000UL;  // 5 min
    static constexpr unsigned long RETRY_INTERVAL  = 60000UL;   // after a failure
    static constexpr unsigned long CONNECT_TIMEOUT = 4000UL;
    static constexpr uint32_t      MIN_TLS_HEAP    = 18000;
    static constexpr uint32_t      MIN_TLS_BLOCK   = 8192;

    // Low heap or no contiguous block: BearSSL cannot do the handshake and
    // the core raises an Exception instead of returning an error.
    static bool memoryOk() {
        return ESP.getFreeHeap() >= MIN_TLS_HEAP &&
               ESP.getMaxFreeBlockSize() >= MIN_TLS_BLOCK;
    }

private:
    Config&       _cfg;
    Logger&       _log;
    Clock&        _clock;
    ReverseDNS    _rdns;
    unsigned long _lastCheck = 0;
    bool          _checked   = false;
    bool          _failing   = false;

    void _check(unsigned long now) {
        char ip[16];
        if (!memoryOk()) {
            _fail(PSTR("Public IP: not enough memory"));
            return;
        }
        BearSSL::X509List trust;
        cloudflareRoots(trust);
        for (uint8_t i = 0;; i++) {
            if (i == NUM_IP_SERVICES) {
                _fail(PSTR("Public IP: no service answered"));
                return;
            }
            if (_query(trust, IP_SERVICES[i][0], IP_SERVICES[i][1], ip)) break;
            yield();
        }

        _failing = false;
        _cfg.ipChecked = now;
        if (strcmp(ip, _cfg.publicIP) == 0) return;
        if (_cfg.publicIP[0])
            _log.log(PSTR("Public IP changed: %s -> %s"), _cfg.publicIP, ip);
        else
            _log.log(PSTR("Public IP: %s"), ip);
        strlcpy(_cfg.publicIP, ip, sizeof(_cfg.publicIP));
        _cfg.ipChanged = now;
        _rdns.lookup(ip, _cfg.publicHost, sizeof(_cfg.publicHost), now);
    }

    // Only the first failure after a success goes to the log
    void _fail(PGM_P msg) {
        if (!_failing) _log.log(msg);
        _failing = true;
    }

    bool _query(const BearSSL::X509List& trust, const char* host, const char* path, char* out) {
        TlsClient client;
        client.setTrustAnchors(&trust);
        client.setX509Time(_clock.now());
        if (!client.prepare(CONNECT_TIMEOUT)) return false;

        String body;
        int code = https::request(client, host, "GET", path, nullptr, nullptr, CONNECT_TIMEOUT, &body, 64);
        body.trim();
        IPAddress ip;
        if (code != 200 || !ip.fromString(body)) return false;
        strlcpy(out, ip.toString().c_str(), 16);
        return true;
    }
};
