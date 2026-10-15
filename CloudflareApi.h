// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include "Config.h"
#include "Clock.h"
#include "CloudflareCA.h"
#include "JsonStream.h"
#include "PublicIPResolver.h"  // memoryOk
#include "TlsClient.h"

// One request to the Cloudflare API (api.cloudflare.com, certificate
// checked, Bearer token), shared by the DDNS update and the setup from the
// token. The answer streams through JsonStream: what matters is pulled out
// as it arrives, so a long zone list never sits in RAM.
namespace cfapi {

constexpr unsigned long CALL_MS   = 8000UL;
constexpr uint8_t       MAX_ZONES = 20;

// One zone of a list
struct Zone {
    char id[33];
    char account[33];
    char name[64];
};

// What an answer carries
struct Answer {
    int8_t  success = -1;       // -1 absent, 1 only for true
    char    message[64] = "";   // errors[0].message
    char    id[33] = "";        // result.id, or the first result's id
    Zone    z = {};             // the zone being read
    File*   out = nullptr;      // where a zone list goes, a line per zone
    uint8_t zones = 0;
    char    account[33] = "";   // the first zone's account
};

inline bool _isId(const char* v) { return strlen(v) == 32 && strspn(v, "0123456789abcdef") == 32; }

inline void _value(void* ctx, const JsonStream& s, const char* v) {
    Answer& a = *static_cast<Answer*>(ctx);
    static const char RESULT[] PROGMEM = "result";
    static const char ID[] PROGMEM = "id";
    uint8_t d = s.depth();
    if (!v) {   // an object ended: a whole zone of the list
        if (a.out && d == 3 && s.is(1, RESULT) && a.z.id[0] && a.z.name[0] && a.zones < MAX_ZONES) {
            char line[sizeof(Zone)];
            int n = snprintf(line, sizeof(line), "%s %s %s\n", a.z.id, a.z.account, a.z.name);
            a.out->write(reinterpret_cast<const uint8_t*>(line), (size_t)n);
            if (!a.zones++) strlcpy(a.account, a.z.account, sizeof(a.account));
        }
        if (d == 3) a.z = Zone{};
        return;
    }
    if (d == 1 && s.is(1, PSTR("success"))) {
        a.success = strcmp(v, "true") == 0;
    } else if (d == 3 && s.is(1, PSTR("errors")) && s.is(3, PSTR("message"))) {
        if (!a.message[0]) strlcpy(a.message, v, sizeof(a.message));
    } else if (!s.is(1, RESULT) || s.cut()) {
        // other members, or text too long to be what is wanted
    } else if (d == 3 && s.is(3, PSTR("name"))) {
        strlcpy(a.z.name, v, sizeof(a.z.name));

    } else if (!_isId(v)) {
        // an ID is 32 hex digits: anything else is not one
    } else if (s.is(d, ID) && d <= 3) {
        if (!a.id[0]) strlcpy(a.id, v, sizeof(a.id));
        strlcpy(a.z.id, v, sizeof(a.z.id));
    } else if (d == 4 && s.is(3, PSTR("account")) && s.is(4, ID)) {
        strlcpy(a.z.account, v, sizeof(a.z.account));
    }
}

inline void _feed(void* ctx, char c) { static_cast<JsonStream*>(ctx)->feed(c); }

// The call; true if Cloudflare said success. Otherwise why (64 bytes) says
// what went wrong: no memory, no connection, the API's own message.
inline bool call(const Config& cfg, const Clock& clock, const char* method, const char* path,
                 const https::Part* parts, size_t count, Answer& a, char* why,
                 const char* type = "application/json") {
    if (!PublicIPResolver::memoryOk()) {
        strncpy_P(why, PSTR("not enough memory"), 63);
        return false;
    }
    BearSSL::X509List trust;
    cloudflareRoots(trust);
    TlsClient client;
    client.setTrustAnchors(&trust);
    client.setX509Time(clock.now());
    if (!client.prepare(CALL_MS)) {
        strncpy_P(why, PSTR("TLS unavailable"), 63);
        return false;
    }
    char head[sizeof("Authorization: Bearer \r\nContent-Type: \r\n") + sizeof(cfg.cf_token) + 64];
    snprintf_P(head, sizeof(head), PSTR("Authorization: Bearer %s\r\nContent-Type: %s\r\n"), cfg.cf_token, type);
    JsonStream js(_value, &a);
    int code = https::exchange(client, "api.cloudflare.com", method, path, head, parts, count, CALL_MS, _feed, &js);
    if (code <= 0) snprintf_P(why, 64, PSTR("no connection (%d)"), code);
    else if (a.success == 1) return true;
    else if (a.message[0]) strlcpy(why, a.message, 64);
    else snprintf_P(why, 64, PSTR("HTTP %d"), code);
    return false;
}

// The same with a body in RAM (may be null)
inline bool call(const Config& cfg, const Clock& clock, const char* method, const char* path,
                 const char* body, Answer& a, char* why) {
    const https::Part part{body, body ? strlen(body) : 0, false};
    return call(cfg, clock, method, path, &part, body ? 1 : 0, a, why);
}

}  // namespace cfapi
