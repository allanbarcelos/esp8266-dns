// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include "App.h"
#include "TlsTrim.h"  // só aqui: substitui partes do BearSSL no link

App app;

void setup() { app.begin(); }

void loop() { app.tick(); }
