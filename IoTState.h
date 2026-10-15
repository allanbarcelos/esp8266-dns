// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <LittleFS.h>
#include "Json.h"

// The actuator's last command, for "on power-up: last state". It lives
// outside config.json: a command writes only this small file, and a config
// saved from the panel does not erase the state. A running pulse is never
// saved (a lock must not open by itself after a power cut).
struct IoTState {
    bool     manual = false;
    bool     on     = false;
    uint8_t  level  = 100;
    uint8_t  angle  = 90;
    uint32_t rgb    = 0xFFFFFF;

    static constexpr const char* PATH = "/iot.state";
    // The largest file written has 55 characters
    static constexpr size_t MAX_TEXT = 96;

    // A broken or foreign file leaves every field as it was; a field of the
    // wrong type or out of range keeps its default
    bool load() {
        File f = LittleFS.open(PATH, "r");
        if (!f) return false;
        char text[MAX_TEXT];
        text[f.readBytes(text, sizeof(text) - 1)] = '\0';
        f.close();
        IoTState st;
        if (!json::object(text, _member, &st)) return false;
        *this = st;
        return true;
    }

    bool save() const {
        File f = LittleFS.open(PATH, "w");
        if (!f) return false;
        json::Writer w(&f);
        w.boolean(PSTR("m"), manual);
        w.boolean(PSTR("on"), on);
        w.num(PSTR("lv"), level);
        w.num(PSTR("an"), angle);
        w.unum(PSTR("c"), rgb);
        w.end();
        f.close();
        return true;
    }

private:
    static void _member(void* ctx, const char* key, size_t len, const json::Value& v) {
        IoTState& s = *static_cast<IoTState*>(ctx);
        uint32_t n;
        bool flag = v.kind == json::BOOL;
        if (json::is(key, len, PSTR("m"))) {
            if (flag) s.manual = v.mag;
        } else if (json::is(key, len, PSTR("on"))) {
            if (flag) s.on = v.mag;
        } else if (json::is(key, len, PSTR("lv"))) {
            if (v.fits(0, 255, n)) s.level = (uint8_t)n;
        } else if (json::is(key, len, PSTR("an"))) {
            if (v.fits(0, 255, n)) s.angle = (uint8_t)n;
        } else if (json::is(key, len, PSTR("c")) && v.fits(0, 0xFFFFFFFFu, n)) {
            s.rgb = n;
        }
    }
};
