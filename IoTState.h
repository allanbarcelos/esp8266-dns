// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>

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
    // 5 fields; when reading, the names stay in the text that was read
    static constexpr size_t CAPACITY = JSON_OBJECT_SIZE(5);
    // The largest file written has 55 characters
    static constexpr size_t MAX_TEXT = 96;

    bool load() {
        File f = LittleFS.open(PATH, "r");
        if (!f) return false;
        // Read whole and parsed in place (char*), like config.json
        char text[MAX_TEXT];
        text[f.readBytes(text, sizeof(text) - 1)] = '\0';
        f.close();
        StaticJsonDocument<CAPACITY> doc;
        DeserializationError err = deserializeJson(doc, text);
        if (err) return false;
        manual = doc["m"] | false;
        on     = doc["on"] | false;
        level  = doc["lv"] | 100;
        angle  = doc["an"] | 90;
        rgb    = doc["c"] | 0xFFFFFFUL;
        return true;
    }

    bool save() const {
        File f = LittleFS.open(PATH, "w");
        if (!f) return false;
        StaticJsonDocument<CAPACITY> doc;
        doc["m"]  = manual;
        doc["on"] = on;
        doc["lv"] = level;
        doc["an"] = angle;
        doc["c"]  = rgb;
        serializeJson(doc, f);
        f.close();
        return true;
    }
};
