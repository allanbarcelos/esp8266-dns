// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Fluxo qualquer do tópico de comandos do ntfy, depois de o servidor
// responder 200: linhas de JSON, lixo, linhas longas, bytes nulos.
#include "../test_support.h"
#include "Clock.h"
#include "Config.h"
#include "IoTService.h"
#include "NtfyNotifier.h"

static time_t fakeTime() { return Clock::MIN_VALID_EPOCH + 100; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    resetMocks();
    Logger log;
    Config cfg;
    strlcpy(cfg.ntfy_cmd, "cmd-abc", sizeof(cfg.ntfy_cmd));
    strlcpy(cfg.iot.actuator, "rele", sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;

    Clock            clock{fakeTime};
    PublicIPResolver resolver{log, clock};
    IoTService       iot{cfg, log};
    iot.begin();
    NtfyNotifier     ntfy{cfg, log, resolver, clock, &iot};

    MockHTTP::keepOpen = true;
    MockHTTP::rawHttp = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n\r\n";
    ntfy.tick(0);
    ntfy.tick(1);
    if (!MockTLS::live) return 0;
    MockTLS::live->inject(std::string(reinterpret_cast<const char*>(data), size));
    for (unsigned long t = 2; t < 64; t++) ntfy.tick(t);
    return 0;
}
