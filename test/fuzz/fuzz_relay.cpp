// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// What the Worker sends down the relay connection (RelayService.h): frames
// of any shape, requests included, served by a real HttpServer.
#include "../test_support.h"
#include "Clock.h"
#include "RelayService.h"

static time_t fuzzTime() { return Clock::MIN_VALID_EPOCH + 100; }

static void answer(void* self) {
    static_cast<HttpServer*>(self)->send(200, "text/plain", "ok");
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    resetMocks();
    Logger log;
    Config cfg;
    strlcpy(cfg.relay_host, "r.example", sizeof(cfg.relay_host));
    strlcpy(cfg.relay_secret, "00", sizeof(cfg.relay_secret));
    cfg.relay_ver = RelayService::PROTOCOL;
    Clock clock(fuzzTime);
    HttpServer http(80);
    http.begin(answer, &http);
    RelayService relay(cfg, log, clock, http);
    MockHTTP::rawHttp = "HTTP/1.1 101 x\r\n\r\n";
    relay.tick(0);
    if (!relay.up()) return 0;
    MockTLS::live->inject(std::string(reinterpret_cast<const char*>(data), size));
    for (unsigned long t = 1; t < 64 && relay.up(); t++) relay.tick(t);
    return 0;
}
