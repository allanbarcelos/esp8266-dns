// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Any stream from the ntfy command topic, after the server
// answers 200: JSON lines, junk, long lines, null bytes.
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
    strlcpy(cfg.iot.actuator, "relay", sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;

    Clock        clock{fakeTime};
    IoTService   iot{cfg, log};
    iot.begin();
    NtfyNotifier ntfy{cfg, log, clock, &iot};

    MockHTTP::keepOpen = true;
    MockHTTP::rawHttp = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n\r\n";
    ntfy.tick(0);
    ntfy.tick(1);
    if (!MockTLS::live) return 0;
    MockTLS::live->inject(std::string(reinterpret_cast<const char*>(data), size));
    for (unsigned long t = 2; t < 64; t++) ntfy.tick(t);
    return 0;
}
