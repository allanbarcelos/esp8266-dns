// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Any config.json and iot.state in flash (broken file, older or newer
// version, swapped types), followed by an IoT boot.
#include "../test_support.h"
#include "Config.h"
#include "ConfigStore.h"
#include "IoTService.h"
#include "IoTState.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    resetMocks();
    std::string text(reinterpret_cast<const char*>(data), size);
    Logger log;
    Config cfg;
    ConfigStore store{cfg, log};
    MockFS::put(ConfigStore::PATH, text);
    MockFS::put(IoTState::PATH, text);
    store.load();
    IoTState st;
    st.load();
    IoTService iot{cfg, log};
    iot.begin();
    iot.tick(1);
    char json[384];
    iot.stateJson(json, sizeof(json));
    return 0;
}
