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

// Cloudflare from the API token alone: which domains (zones) it reaches,
// and the DDNS record by name, created if it does not exist. No IDs to copy
// from the dashboard.
//
// The panel only asks; the work runs from tick(), one job per tick, so the
// page answers at once and the TLS session is free (App holds the other
// sessions while due()). The zone list goes to ZONES_PATH, a line per zone
// ("id account name"), for the panel's select.
class CloudflareSetup {
public:
    enum Job : uint8_t { NONE, SCAN, RECORD };
    enum State : uint8_t { IDLE, WORKING, DONE, FAILED };

    static constexpr const char* ZONES_PATH = "/cf.zones";

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

    // The next tick will use TLS
    bool due() const { return _job != NONE && WiFi.status() == WL_CONNECTED && _clock.synced(); }

    void tick(unsigned long) {
        if (!due()) return;
        uint8_t j = _job;
        _job = NONE;
        bool ok = j == SCAN ? _scan() : _record();
        if (!ok) _log.log(PSTR("Cloudflare: %s"), _message);
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
            cfapi::Answer b;
            if (!cfapi::call(_cfg, _clock, "GET", "/client/v4/accounts?per_page=1", nullptr, b, _message)) return false;
            strlcpy(_cfg.cf_account, b.id, sizeof(_cfg.cf_account));
        }
        _store.save();
        _log.log(PSTR("Cloudflare: token reaches %u zone(s)"), (unsigned)a.zones);
        return true;
    }

    // The record by name, or a new one pointing at the current IP
    bool _record() {
        Zone z;
        if (!find(_zone, z)) return _fail(PSTR("unknown zone"));
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
};
