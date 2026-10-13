// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Bytes quaisquer vindos do broker MQTT. O primeiro byte escolhe se antes
// chega um CONNACK válido (o parser de PUBLISH só roda conectado).
#include "../test_support.h"
#include "Config.h"
#include "IoTService.h"
#include "MqttService.h"

struct Broker : WiFiClient {
    std::string in;
    size_t      pos = 0;
    bool        up  = false;

    int    available() override { return (int)(in.size() - pos); }
    int    read() override { return pos < in.size() ? (uint8_t)in[pos++] : -1; }
    size_t write(const uint8_t*, size_t n) override { return n; }
    bool   connected() override { return up; }
    int    connect(const char*, uint16_t) override { up = true; return 1; }
    void   stop(unsigned int = 0) override { up = false; }
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    resetMocks();
    Logger log;
    Config cfg;
    strlcpy(cfg.mqtt_host, "10.0.0.1", sizeof(cfg.mqtt_host));
    strlcpy(cfg.iot.actuator, (data[0] & 2) ? "fita" : "rele", sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;
    strlcpy(cfg.iot.sensor, "pir", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    IoTService iot{cfg, log};
    iot.begin();
    Broker net;
    MqttService mqtt{cfg, log, iot, net};

    mqtt.tick(0);
    if (data[0] & 1) net.in.assign("\x20\x02\x00\x00", 4);
    net.in.append(reinterpret_cast<const char*>(data) + 1, size - 1);
    for (unsigned long t = 1; t < 64 && net.available() > 0; t++) mqtt.tick(t);
    return 0;
}
