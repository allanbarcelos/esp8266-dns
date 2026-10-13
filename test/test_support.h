// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
// Utilitários compartilhados pelas suítes de teste nativas.
#include <lwip/udp.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <LittleFS.h>
#include <Updater.h>
#include <cstring>
#include "Logger.h"
#include "CloudflareCA.h"

// malloc falha uma vez para o tamanho pedido (link com -Wl,--wrap=malloc
// no env native). Só o tamanho exato falha: libc e Unity seguem normais.
namespace mock { inline size_t failMallocSize = 0; }
extern "C" void* __real_malloc(size_t n);
extern "C" void* __wrap_malloc(size_t n) {
    if (mock::failMallocSize != 0 && n == mock::failMallocSize) {
        mock::failMallocSize = 0;
        return nullptr;
    }
    return __real_malloc(n);
}

// Restaura todos os mocks globais para o estado inicial
inline void resetMocks() {
    setMillis(0);
    MockHTTP::reset();
    MockTLS::reset();
    MockFS::reset();
    WiFi.reset();
    ESP.reset();
    Update.reset();
    MockUDP::reset();
    mock::configTimeCalls = 0;
    mock::failMallocSize = 0;
    Serial.last.clear();
    Serial.printfCalls = 0;
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

// A conexão confia exatamente nas raízes de CloudflareCA.h, uma por append
inline bool cloudflareTrust(const MockTLS::Trust& t) {
    return t.first == CLOUDFLARE_DER
        && t.count == sizeof(CLOUDFLARE_LEN) / sizeof(CLOUDFLARE_LEN[0])
        && t.bytes == sizeof(CLOUDFLARE_DER);
}
