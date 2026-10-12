// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>

// Último comando do atuador, para "ao religar: último estado". Fica fora do
// config.json: um comando grava só este arquivo pequeno, e um config salvo
// pela página não apaga o estado. Pulso em andamento nunca é gravado (uma
// fechadura não pode reabrir sozinha depois de uma queda de energia).
struct IoTState {
    bool     manual = false;
    bool     on     = false;
    uint8_t  level  = 100;
    uint8_t  angle  = 90;
    uint32_t rgb    = 0xFFFFFF;

    static constexpr const char* PATH = "/iot.state";
    // 5 campos e os nomes copiados do arquivo na leitura
    static constexpr size_t CAPACITY = JSON_OBJECT_SIZE(5) + 16;

    bool load() {
        File f = LittleFS.open(PATH, "r");
        if (!f) return false;
        StaticJsonDocument<CAPACITY> doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
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
