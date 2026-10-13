// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Texto de comando qualquer (painel, API, ntfy, MQTT), em cada capacidade
// de atuador, e o JSON de estado que sai depois.
#include "../test_support.h"
#include "Config.h"
#include "IoTCommand.h"
#include "IoTService.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string text(reinterpret_cast<const char*>(data), size);
    iot::Command c;
    iot::parseCommand(text.c_str(), c);

    static const char* ACTUATORS[] = {"rele", "led", "servo", "fita"};
    for (const char* a : ACTUATORS) {
        resetMocks();
        Logger log;
        Config cfg;
        strlcpy(cfg.iot.actuator, a, sizeof(cfg.iot.actuator));
        cfg.iot.actuatorPin = 0;
        IoTService iot{cfg, log};
        iot.begin();
        iot.command(text.c_str(), 0);
        iot.tick(1);
        char json[384];
        iot.stateJson(json, sizeof(json));
    }
    return 0;
}
