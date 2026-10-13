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
    // 5 campos; na leitura os nomes ficam no texto lido
    static constexpr size_t CAPACITY = JSON_OBJECT_SIZE(5);
    // O maior arquivo gravado tem 55 caracteres
    static constexpr size_t MAX_TEXT = 96;

    bool load() {
        File f = LittleFS.open(PATH, "r");
        if (!f) return false;
        // Lido inteiro e analisado no lugar (char*), como o config.json
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
