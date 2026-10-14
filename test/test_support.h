// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
// Helpers shared by the native test suites.
#include <lwip/udp.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <LittleFS.h>
#include <Updater.h>
#include <cstring>
#include "Logger.h"
#include "CloudflareCA.h"

// malloc fails once for the requested size (linked with -Wl,--wrap=malloc
// in the native env). Only that exact size fails: libc and Unity keep working.
namespace mock { inline size_t failMallocSize = 0; }
extern "C" void* __real_malloc(size_t n);
extern "C" void* __wrap_malloc(size_t n) {
    if (mock::failMallocSize != 0 && n == mock::failMallocSize) {
        mock::failMallocSize = 0;
        return nullptr;
    }
    return __real_malloc(n);
}

// Restores every global mock to its initial state
inline void resetMocks() {
    setMillis(0);
    MockHTTP::reset();
    MockTLS::reset();
    MockServer::reset();
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

// true if any line of the ring buffer contains the text
inline bool logHas(const Logger& log, const char* needle) {
    for (uint8_t i = 0; i < log.count(); i++) {
        if (std::strstr(log.entry(log.startIndex() + i), needle)) return true;
    }
    return false;
}

inline bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The connection trusts exactly the roots in CloudflareCA.h, one per append
inline bool cloudflareTrust(const MockTLS::Trust& t) {
    return t.first == CLOUDFLARE_DER
        && t.count == sizeof(CLOUDFLARE_LEN) / sizeof(CLOUDFLARE_LEN[0])
        && t.bytes == sizeof(CLOUDFLARE_DER);
}
