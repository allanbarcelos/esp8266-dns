// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include "Config.h"
#include "ConfigStore.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareApi.h"
#include "CloudflareDNS.h"
#include "RelayScript.h"

#define RELAY_BOUNDARY "esp8266-relay-part"

// Cloudflare from the API token alone: which domains (zones) it reaches,
// the DDNS record by name, created if it does not exist, and the remote
// access relay's Worker (relay/worker.mjs, embedded in RelayScript.h). No
// IDs to copy from the dashboard.
//
// The panel only asks; the work runs from tick(), one job per tick, so the
// page answers at once and the TLS session is free (App holds the other
// sessions while due()). The zone list goes to ZONES_PATH, a line per zone
// ("id account name"), for the panel's select.
class CloudflareSetup {
    static constexpr char UNKNOWN_ZONE[] PROGMEM = "unknown zone";
public:
    enum Job : uint8_t { NONE, SCAN, RECORD, DEPLOY };
    enum State : uint8_t { IDLE, WORKING, DONE, FAILED };

    static constexpr const char* ZONES_PATH = "/cf.zones";
    static constexpr unsigned long REDEPLOY_MS = 3600000UL;

    using Zone = cfapi::Zone;

    CloudflareSetup(Config& cfg, Logger& log, ConfigStore& store, Clock& clock, CloudflareDNS* dns = nullptr)
        : _cfg(cfg), _log(log), _store(store), _clock(clock), _dns(dns) {}

    // Lists the zones the token reaches
    void scan() { _start(SCAN); }

    // Finds or creates the A record "name" in the zone and makes it the DDNS one
    void record(const char* zone, const char* name) {
        strlcpy(_zone, zone, sizeof(_zone));
        strlcpy(_name, name, sizeof(_name));
        _start(RECORD);
    }

    // The relay Worker, from the firmware, into the account: name is its
    // name and the first label of its address, zone the zone of the address
    // ("" = the account's workers.dev; wd names it if the account has none)
    void deploy(const char* name, const char* zone, const char* wd) {
        strlcpy(_name, name, sizeof(_name));
        strlcpy(_zone, zone, sizeof(_zone));
        strlcpy(_wd, wd, sizeof(_wd));
        _start(DEPLOY);
    }

    // After a firmware update with another relay protocol, the Worker goes
    // up again by itself, at most once an hour
    void redeploy(unsigned long now) {
        if (!_cfg.relay_host[0] || _cfg.relay_ver == RELAY_VERSION || _job != NONE) return;
        if (_redeployed && now - _redeployAt < REDEPLOY_MS) return;
        _redeployed = true;
        _redeployAt = now;
        deploy(_cfg.relay_name, _cfg.relay_zone, "");
    }

    // A deploy just finished: true once
    bool takeDeployed() {
        bool d = _deployed;
        _deployed = false;
        return d;
    }

    // The next tick will use TLS
    bool due() const { return _job != NONE && WiFi.status() == WL_CONNECTED && _clock.synced(); }

    void tick(unsigned long) {
        if (!due()) return;
        uint8_t j = _job;
        _job = NONE;
        bool ok = j == SCAN ? _scan() : j == RECORD ? _record() : _deploy();
        if (!ok) _log.log(PSTR("Cloudflare: %s"), _message);
        else _message[0] = 0;   // a step that may fail (a missing Worker) said why
        _state = ok ? DONE : FAILED;
    }

    State       state() const   { return (State)_state; }
    uint8_t     last() const    { return _last; }      // the job of state()
    uint8_t     zones() const   { return _zones; }
    const char* message() const { return _message; }   // why it failed

    // The zone with this id from the list (false if not there)
    static bool find(const char* id, Zone& z) {
        File f = LittleFS.open(ZONES_PATH, "r");
        bool found = false;
        while (f && !found && next(f, z)) found = strcmp(z.id, id) == 0;
        if (f) f.close();
        return found;
    }

    // The next zone of the open list; false at its end
    static bool next(File& f, Zone& z) {
        for (;;) {
            char line[sizeof(Zone)];
            size_t n = 0;
            int c;
            while ((c = f.read()) >= 0 && c != '\n')
                if (n + 1 < sizeof(line)) line[n++] = (char)c;
            line[n] = '\0';
            if (c < 0 && n == 0) return false;
            // "id account name": the IDs are 32 hex digits
            if (n < 67 || line[32] != ' ' || line[65] != ' ') continue;
            memcpy(z.id, line, 32);
            z.id[32] = '\0';
            memcpy(z.account, line + 33, 32);
            z.account[32] = '\0';
            strlcpy(z.name, line + 66, sizeof(z.name));
            return true;
        }
    }

private:
    Config&        _cfg;
    Logger&        _log;
    ConfigStore&   _store;
    Clock&         _clock;
    CloudflareDNS* _dns;
    uint8_t        _job = NONE, _state = IDLE, _last = NONE, _zones = 0;
    char           _message[64] = "";
    char           _zone[33] = "";
    char           _name[sizeof(Config::cf_host)] = "";
    char           _wd[64] = "";              // workers.dev name for an account without one
    bool           _deployed = false, _redeployed = false;
    unsigned long  _redeployAt = 0;

    void _start(uint8_t j) {
        _job = _last = j;
        _state = WORKING;
        _message[0] = '\0';
    }

    bool _fail(PGM_P why) {
        strncpy_P(_message, why, sizeof(_message) - 1);
        return false;
    }

    // Zones into the file; with none, the account comes from the account list
    bool _scan() {
        File f = LittleFS.open(ZONES_PATH, "w");
        if (!f) return _fail(PSTR("cannot write the zone list"));
        cfapi::Answer a;
        a.out = &f;
        bool ok = cfapi::call(_cfg, _clock, "GET", "/client/v4/zones?per_page=20&status=active", nullptr, a, _message);
        f.close();
        if (!ok) return false;
        _zones = a.zones;
        if (a.zones) {
            strlcpy(_cfg.cf_account, a.account, sizeof(_cfg.cf_account));
        } else {
            cfapi::reset(a);
            if (!cfapi::call(_cfg, _clock, "GET", "/client/v4/accounts?per_page=1", nullptr, a, _message)) return false;
            strlcpy(_cfg.cf_account, a.id, sizeof(_cfg.cf_account));
        }
        _store.save();
        _log.log(PSTR("Cloudflare: token reaches %u zone(s)"), (unsigned)_zones);
        return true;
    }

    // The record by name, or a new one pointing at the current IP
    bool _record() {
        Zone z;
        if (!find(_zone, z)) return _fail(UNKNOWN_ZONE);
        char path[sizeof("/client/v4/zones//dns_records?type=A&name=") + sizeof(_zone) + sizeof(_name)];
        int n = snprintf_P(path, sizeof(path), PSTR("/client/v4/zones/%s/dns_records"), _zone);
        snprintf_P(path + n, sizeof(path) - n, PSTR("?type=A&name=%s"), _name);
        cfapi::Answer a;
        if (!cfapi::call(_cfg, _clock, "GET", path, nullptr, a, _message)) return false;
        if (!a.id[0]) {
            char body[sizeof("{\"type\":\"A\",\"name\":\"\",\"content\":\"\",\"ttl\":1,\"proxied\":false}") +
                      sizeof(_name) + sizeof(_cfg.publicIP)];
            snprintf_P(body, sizeof(body),
                       PSTR("{\"type\":\"A\",\"name\":\"%s\",\"content\":\"%s\",\"ttl\":1,\"proxied\":false}"),
                       _name, _cfg.publicIP[0] ? _cfg.publicIP : "192.0.2.1");
            path[n] = '\0';
            if (!cfapi::call(_cfg, _clock, "POST", path, body, a, _message)) return false;
            _log.log(PSTR("Cloudflare: record %s created"), _name);
        }
        strlcpy(_cfg.cf_zone, z.id, sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_account, z.account, sizeof(_cfg.cf_account));
        strlcpy(_cfg.cf_record, a.id, sizeof(_cfg.cf_record));
        strlcpy(_cfg.cf_host, _name, sizeof(_cfg.cf_host));
        _store.save();
        if (_dns) _dns->resync();
        return true;
    }

    // The Worker, expanded from its byte pairs as it goes out (RelayScript.h)
    static void _script(WiFiClient& c) {
        uint8_t out[64];
        uint8_t stack[RELAY_DEPTH + 2];
        size_t n = 0;
        for (size_t i = 0; i < sizeof(RELAY_SCRIPT); i++) {
            uint8_t k = 0;
            stack[k++] = pgm_read_byte(&RELAY_SCRIPT[i]);
            while (k) {
                uint8_t b = stack[--k];
                // The code's rank: 0x7F up first, then the control bytes but the newline
                int r = b >= 0x7F ? b - 0x7F : (b < 0x20 && b != '\n') ? 129 + b - (b > '\n') : -1;
                if (r < 0) {
                    out[n++] = b;
                    if (n == sizeof(out)) {
                        c.write(out, n);
                        n = 0;
                    }
                } else {
                    stack[k++] = pgm_read_byte(&RELAY_PAIRS[2 * r + 1]);
                    stack[k++] = pgm_read_byte(&RELAY_PAIRS[2 * r]);
                }
            }
        }
        c.write(out, n);
    }

    // The upload: metadata (the secret, the Durable Object and, on the first
    // upload, its migration) and the script, as multipart/form-data
    bool _upload(const char* acc, bool first, const char* secret, cfapi::Answer& a) {
        static const char META_A[] PROGMEM =
            "--" RELAY_BOUNDARY "\r\nContent-Disposition: form-data; name=\"metadata\"\r\n"
            "Content-Type: application/json\r\n\r\n"
            "{\"main_module\":\"worker.mjs\",\"compatibility_date\":\"2025-09-01\",\"bindings\":["
            "{\"type\":\"secret_text\",\"name\":\"SECRET\",\"text\":\"";
        static const char META_B[] PROGMEM =
            "\"},{\"type\":\"durable_object_namespace\",\"name\":\"DEVICE\",\"class_name\":\"Device\"}]";
        static const char MIGRATION[] PROGMEM = ",\"migrations\":{\"new_tag\":\"v1\",\"new_sqlite_classes\":[\"Device\"]}";
        static const char META_C[] PROGMEM =
            "}\r\n--" RELAY_BOUNDARY "\r\nContent-Disposition: form-data; name=\"worker.mjs\"; filename=\"worker.mjs\"\r\n"
            "Content-Type: application/javascript+module\r\n\r\n";
        static const char TAIL[] PROGMEM = "\r\n--" RELAY_BOUNDARY "--\r\n";
        const https::Part parts[] = {
            {META_A, sizeof(META_A) - 1, true},
            {secret, strlen(secret), false},
            {META_B, sizeof(META_B) - 1, true},
            {MIGRATION, first ? sizeof(MIGRATION) - 1 : 0, true},
            {META_C, sizeof(META_C) - 1, true},
            {nullptr, RELAY_SCRIPT_LEN, false, _script},
            {TAIL, sizeof(TAIL) - 1, true},
        };
        char path[sizeof("/client/v4/accounts//workers/scripts/") + 32 + sizeof(_name)];
        snprintf_P(path, sizeof(path), PSTR("/client/v4/accounts/%s/workers/scripts/%s"), acc, _name);
        cfapi::reset(a);
        return cfapi::call(_cfg, _clock, "PUT", path, parts, sizeof(parts) / sizeof(parts[0]), a, _message,
                           "multipart/form-data; boundary=" RELAY_BOUNDARY);
    }

    static constexpr size_t HOST_LEN = sizeof(_name) + sizeof(Zone::name) + sizeof(".workers.dev");

    // The helpers below keep their buffers off _deploy's frame, which is
    // still on the stack during the upload, the deepest call

    // The account's workers.dev name, created if it has none; host gets the
    // relay's address there. path holds the account's prefix (n characters).
    __attribute__((noinline)) bool _workersDev(char* path, int n, char* host, cfapi::Answer& a) {
        strcpy_P(path + n, PSTR("subdomain"));
        cfapi::reset(a);
        if (!cfapi::call(_cfg, _clock, "GET", path, nullptr, a, _message) || !a.sub[0]) {
            if (!_wd[0]) return _fail(PSTR("the account needs a workers.dev name"));
            char body[sizeof("{\"subdomain\":\"\"}") + sizeof(_wd)];
            snprintf_P(body, sizeof(body), PSTR("{\"subdomain\":\"%s\"}"), _wd);
            cfapi::reset(a);
            if (!cfapi::call(_cfg, _clock, "PUT", path, body, a, _message)) return false;
            strlcpy(a.sub, _wd, sizeof(a.sub));
        }
        snprintf(host, HOST_LEN, "%s.%s.workers.dev", _name, a.sub);
        return true;
    }

    // The address: the zone's name (zone: its id), or workers.dev (zone null)
    __attribute__((noinline)) bool _route(char* path, size_t cap, int n, const char* host, const char* zone,
                                          cfapi::Answer& a) {
        cfapi::reset(a);
        if (!zone) {
            snprintf_P(path + n, cap - n, PSTR("scripts/%s/subdomain"), _name);
            return cfapi::call(_cfg, _clock, "POST", path, "{\"enabled\":true,\"previews_enabled\":false}", a, _message);
        }
        strcpy_P(path + n, PSTR("domains"));
        char body[sizeof("{\"hostname\":\"\",\"service\":\"\",\"zone_id\":\"\"}") + HOST_LEN + sizeof(_name) + 32];
        snprintf_P(body, sizeof(body), PSTR("{\"hostname\":\"%s\",\"service\":\"%s\",\"zone_id\":\"%s\"}"),
                   host, _name, zone);
        return cfapi::call(_cfg, _clock, "PUT", path, body, a, _message);
    }

    bool _deploy() {
        Zone z = {};
        bool own = _zone[0];
        if (own && !find(_zone, z)) return _fail(UNKNOWN_ZONE);
        const char* acc = own ? z.account : _cfg.cf_account;
        if (!cfapi::_isId(acc)) return _fail(PSTR("no account: save and check the token"));

        // A Worker by that name must be a relay already, or not exist. One
        // Answer for every call: the loop's stack is 4 KB, and this chain
        // (down to the TLS handshake and the interrupts on top) nearly fills it
        char path[sizeof("/client/v4/accounts//workers/scripts//settings") + 32 + sizeof(_name)];
        int n = snprintf_P(path, sizeof(path), PSTR("/client/v4/accounts/%s/workers/"), acc);
        snprintf_P(path + n, sizeof(path) - n, PSTR("scripts/%s/settings"), _name);
        cfapi::Answer a;
        bool exists = cfapi::call(_cfg, _clock, "GET", path, nullptr, a, _message);
        if (!exists && a.http != 404) return false;
        if (exists && !a.relay) return _fail(PSTR("another Worker has this name"));

        char host[HOST_LEN];   // room for any, checked below
        if (own) snprintf(host, sizeof(host), "%s.%s", _name, z.name);
        else if (!_workersDev(path, n, host, a)) return false;
        if (strlen(host) >= sizeof(_cfg.relay_host)) return _fail(PSTR("address too long"));

        // A new secret on every upload: only this device and the Worker know it
        char secret[sizeof(_cfg.relay_secret)];
        for (int i = 0; i < 8; i++) snprintf(secret + 8 * i, 9, "%08lx", (unsigned long)ESP.random());
        if (!_upload(acc, !exists, secret, a)) return false;

        if (!_route(path, sizeof(path), n, host, own ? z.id : nullptr, a)) return false;
        strlcpy(_cfg.relay_secret, secret, sizeof(_cfg.relay_secret));
        strlcpy(_cfg.relay_host, host, sizeof(_cfg.relay_host));
        strlcpy(_cfg.relay_name, _name, sizeof(_cfg.relay_name));
        strlcpy(_cfg.relay_zone, _zone, sizeof(_cfg.relay_zone));
        _cfg.relay_ver = RELAY_VERSION;
        _store.save();
        _deployed = true;
        _log.log(PSTR("Cloudflare: relay at %s"), host);
        return true;
    }
};
