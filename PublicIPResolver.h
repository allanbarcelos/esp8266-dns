// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareCA.h"
#include "ReverseDNS.h"

// WiFiClientSecure's connect uses the BearSSL context timeout (_owned).
// HTTPClient::setTimeout only sets the wrapper, and the context keeps the
// core's 15 s: each unreachable host blocks the loop for that long.
class TlsClient : public WiFiClientSecure {
public:
    // BearSSL's default buffer is 16 KB. This device has ~30 KB of heap
    // left: that allocation fragments memory, the watchdog restarts the ESP
    // and, without a contiguous block, the handshake raises an Exception.
    static constexpr uint16_t RX_BUF = 4096;
    static constexpr uint16_t TX_BUF = 512;

    // Only ECDHE with AES-GCM, which every server in use accepts
    // (Cloudflare, GitHub, ntfy.sh, ipify, icanhazip, ifconfig.co). The other
    // ciphers are not even in the firmware (TlsTrim.h): announcing one would
    // let the server pick something the device cannot do.
    static constexpr int SUITE_COUNT = 4;

    bool prepare(unsigned long ms, uint16_t rx = RX_BUF, uint16_t tx = TX_BUF) {
        if (!_owned) return false;
        setTimeout(ms);
        _owned->setTimeout(ms);
        setBufferSizes(rx, tx);
        return setCiphers(_SUITES, SUITE_COUNT);
    }

private:
    // ECDHE_ECDSA and ECDHE_RSA, AES_128_GCM_SHA256 and AES_256_GCM_SHA384
    static constexpr uint16_t _SUITES[SUITE_COUNT] = {0xC02B, 0xC02F, 0xC02C, 0xC030};
};

// Public IP over HTTPS. The three providers return only the IPv4 and use
// the WE1 → GTS Root R4 chain, the same one api.cloudflare.com already
// validates in the field with the roots in CloudflareCA.h. A service outside
// that chain is not used: TLS would fail on this device.
//
// Checks every CHECK_INTERVAL, with or without DNS and ntfy set up; the
// first valid IPv4 answer ends the search (one TLS session per cycle). The
// result goes to Config: DNS, ntfy and the panel read it from there. The
// log only gets changes: a new IP, or starting to fail.
static const char _IP_SVC_0[] PROGMEM = "https://api4.ipify.org";
static const char _IP_SVC_1[] PROGMEM = "https://icanhazip.com";
static const char _IP_SVC_2[] PROGMEM = "https://ifconfig.co/ip";

static const char* const IP_SERVICES[] PROGMEM = {
    _IP_SVC_0, _IP_SVC_1, _IP_SVC_2
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
            char url[48];
            strcpy_P(url, (char*)pgm_read_ptr(&IP_SERVICES[i]));
            if (_query(trust, url, ip)) break;
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

    bool _query(const BearSSL::X509List& trust, const char* url, char* out) {
        TlsClient client;
        client.setTrustAnchors(&trust);
        client.setX509Time(_clock.now());
        if (!client.prepare(CONNECT_TIMEOUT)) return false;

        HTTPClient http;
        http.setTimeout(CONNECT_TIMEOUT);
        if (!http.begin(client, url)) return false;
        http.addHeader("User-Agent", "ESP8266");

        int code = http.GET();
        String body = (code == HTTP_CODE_OK) ? http.getString() : String();
        http.end();

        body.trim();
        IPAddress ip;
        if (code != HTTP_CODE_OK || !ip.fromString(body)) return false;
        strlcpy(out, ip.toString().c_str(), 16);
        return true;
    }
};
