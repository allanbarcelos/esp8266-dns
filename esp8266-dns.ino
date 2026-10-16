// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include "App.h"
#include "TlsTrim.h"  // only here: replaces parts of BearSSL at link time

App app;

void setup() { app.begin(); }

void loop() { app.tick(); }

// An exception or the software watchdog: the core calls this before the
// reboot, and the next boot logs where the loop was
extern "C" void custom_crash_callback(struct rst_info*, uint32_t stack, uint32_t end) {
    app.net.crashed(reinterpret_cast<const uint32_t*>(stack), reinterpret_cast<const uint32_t*>(end));
}
