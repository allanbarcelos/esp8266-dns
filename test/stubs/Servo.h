// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
// Substitui o Servo.h do core ESP8266 nos testes nativos.
#include <Arduino.h>

class Servo {
public:
    uint8_t attach(int pin) {
        mock::servoPin = pin;
        mock::servoAttached = mock::servoAttachResult != 0;
        return (uint8_t)mock::servoAttachResult;
    }
    void write(int angle) {
        mock::servoAngle = angle;
        mock::servoWrites++;
    }
    void detach() { mock::servoAttached = false; }
    bool attached() const { return mock::servoAttached; }
};
