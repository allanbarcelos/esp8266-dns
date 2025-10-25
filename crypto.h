// crypto.h
#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>

void decryptBuffer(uint8_t* buf, size_t len) {
    uint32_t chipId = ESP.getChipId();
    for (size_t i = 0; i < len; i++) {
        buf[i] ^= (chipId & 0xFF);
    }
}
