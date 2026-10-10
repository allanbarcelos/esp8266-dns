#pragma once
// Utilitários compartilhados pelas suítes de teste nativas.
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <LittleFS.h>
#include <Updater.h>
#include <cstring>
#include "Logger.h"

// Restaura todos os mocks globais para o estado inicial
inline void resetMocks() {
    setMillis(0);
    MockHTTP::reset();
    MockTLS::reset();
    MockFS::reset();
    WiFi.reset();
    ESP.reset();
    Update.reset();
    mock::configTimeCalls = 0;
    mock::ntpServer1.clear();
    mock::resetPins();
    mock::resetBus();
}

// true se alguma linha do buffer circular contém o trecho
inline bool logHas(const Logger& log, const char* needle) {
    for (uint8_t i = 0; i < log.count(); i++) {
        if (std::strstr(log.entry(log.startIndex() + i), needle)) return true;
    }
    return false;
}

inline bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}
