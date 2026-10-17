// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include "I18n.h"

// Catalog of sensors and actuators. The configuration stores the identifier
// (slug), never the position in the table: the order and the display names
// can change freely. A published slug is never renamed or reused.
//
// A new module that uses an existing capability is a single line in the
// tables: panel, commands, API and ntfy work with no other code. A new
// capability needs a driver and a panel control (IoTService/WebService).
namespace iot {

// How the sensor is read
enum Kind : uint8_t { K_NONE, K_DIGITAL, K_DHT11, K_DHT22, K_DS18, K_RANGE, K_PULSE, K_SHT3X, K_AHT, K_LUX };

// Temperature and humidity together (DHT, SHT3x or AHT20)
inline bool tempAndHumidity(uint8_t k) { return k == K_DHT11 || k == K_DHT22 || k == K_SHT3X || k == K_AHT; }

// On I2C: SDA on the sensor's pin, SCL on its pair
inline bool i2c(uint8_t k) { return k == K_SHT3X || k == K_AHT || k == K_LUX; }

// Units the IoT page offers: none (digital, light), two (a temperature) or
// three (a distance: cm, tank level, tank volume; pulses: count, L/min, mL/min)
inline uint8_t unitCount(uint8_t k) {
    return k == K_DIGITAL || k == K_LUX ? 0 : k == K_RANGE || k == K_PULSE ? 3 : 2;
}

// What the actuator does: sets the panel control and the accepted commands
enum Cap : uint8_t { C_NONE, C_ONOFF, C_LEVEL, C_POSITION, C_COLOR };

struct SensorDef {
    const char* slug;   // RAM: goes to JSON and comparisons
    PGM_P       label;  // flash: only shown on the panel (I18n.h marker)
    uint8_t     kind;
    uint8_t     group;  // group in the panel list
    PGM_P       on;     // digital: name of the active / inactive state
    PGM_P       off;
};

struct ActuatorDef {
    const char* slug;
    PGM_P       label;
    uint8_t     cap;
    uint8_t     pulse;  // pulse length suggested on the panel (s)
};

static const char _S_DHT22[]  PROGMEM = L_S_DHT22;
static const char _S_DHT11[]  PROGMEM = L_S_DHT11;
static const char _S_DS18[]   PROGMEM = L_S_DS18;
static const char _S_SHT3X[]  PROGMEM = L_S_SHT3X;
static const char _S_PIR[]    PROGMEM = L_S_PIR;
static const char _S_DOOR[]   PROGMEM = L_S_DOOR;
static const char _S_BTN[]    PROGMEM = L_S_BUTTON;
static const char _S_DIG[]    PROGMEM = L_S_DIGITAL;
static const char _S_RANGE[]  PROGMEM = L_S_RANGE;
static const char _S_PULSE[]  PROGMEM = L_S_PULSE;
static const char _S_AHT[]    PROGMEM = L_S_AHT;
static const char _S_LUX[]    PROGMEM = L_S_LUX;
static const char _S_LEAK[]   PROGMEM = L_S_LEAK;
static const char _S_GAS[]    PROGMEM = L_S_GAS;
static const char _S_FLOAT[]  PROGMEM = L_S_FLOAT;
static const char _S_RAIN[]   PROGMEM = L_S_RAIN;
static const char _S_RADAR[]  PROGMEM = L_S_RADAR;

static const char _T_MOTION[] PROGMEM = "motion";
static const char _T_CLEAR[]  PROGMEM = "clear";
static const char _T_OPEN[]   PROGMEM = "open";
static const char _T_SHUT[]   PROGMEM = "closed";
static const char _T_PRESS[]  PROGMEM = "pressed";
static const char _T_REL[]    PROGMEM = "released";
static const char _T_ACT[]    PROGMEM = "active";
static const char _T_IDLE[]   PROGMEM = "idle";
static const char _T_WET[]    PROGMEM = "wet";
static const char _T_DRY[]    PROGMEM = "dry";
static const char _T_GAS[]    PROGMEM = "gas";
static const char _T_HIGH[]   PROGMEM = "high";
static const char _T_LOW[]    PROGMEM = "low";
static const char _T_RAIN[]   PROGMEM = "rain";
static const char _T_PRES[]   PROGMEM = "presence";

static const char _A_RELAY[]  PROGMEM = L_A_RELAY;
static const char _A_PUMP[]   PROGMEM = L_A_PUMP;
static const char _A_VALVE[]  PROGMEM = L_A_VALVE;
static const char _A_LOCK[]   PROGMEM = L_A_LOCK;
static const char _A_BUZZ[]   PROGMEM = L_A_BUZZER;
static const char _A_LED[]    PROGMEM = L_A_LED;
static const char _A_FAN[]    PROGMEM = L_A_FAN;
static const char _A_MOTOR[]  PROGMEM = L_A_MOTOR;
static const char _A_SERVO[]  PROGMEM = "Servo";
static const char _A_STRIP[]  PROGMEM = L_A_STRIP;
static const char _A_GATE[]   PROGMEM = L_A_GATE;

static const SensorDef SENSORS[] = {
    {"dht22",    _S_DHT22, K_DHT22,   0, nullptr,   nullptr},
    {"dht11",    _S_DHT11, K_DHT11,   0, nullptr,   nullptr},
    {"ds18b20",  _S_DS18,  K_DS18,    0, nullptr,   nullptr},
    {"sht3x",    _S_SHT3X, K_SHT3X,   0, nullptr,   nullptr},
    {"aht20",    _S_AHT,   K_AHT,     0, nullptr,   nullptr},
    {"pir",      _S_PIR,   K_DIGITAL, 1, _T_MOTION, _T_CLEAR},
    {"radar",    _S_RADAR, K_DIGITAL, 1, _T_PRES,   _T_CLEAR},
    {"contact",  _S_DOOR,  K_DIGITAL, 1, _T_OPEN,   _T_SHUT},
    {"button",   _S_BTN,   K_DIGITAL, 1, _T_PRESS,  _T_REL},
    {"leak",     _S_LEAK,  K_DIGITAL, 1, _T_WET,    _T_DRY},
    {"gas",      _S_GAS,   K_DIGITAL, 1, _T_GAS,    _T_CLEAR},
    {"float",    _S_FLOAT, K_DIGITAL, 1, _T_HIGH,   _T_LOW},
    {"rain",     _S_RAIN,  K_DIGITAL, 1, _T_RAIN,   _T_DRY},
    {"digital",  _S_DIG,   K_DIGITAL, 1, _T_ACT,    _T_IDLE},
    {"distance", _S_RANGE, K_RANGE,   2, nullptr,   nullptr},
    {"pulses",   _S_PULSE, K_PULSE,   2, nullptr,   nullptr},
    {"bh1750",   _S_LUX,   K_LUX,     2, nullptr,   nullptr},
};

static const ActuatorDef ACTUATORS[] = {
    {"relay",  _A_RELAY, C_ONOFF,    1},
    {"pump",   _A_PUMP,  C_ONOFF,    10},
    {"valve",  _A_VALVE, C_ONOFF,    10},
    {"lock",   _A_LOCK,  C_ONOFF,    3},
    {"gate",   _A_GATE,  C_ONOFF,    1},
    {"buzzer", _A_BUZZ,  C_ONOFF,    1},
    {"led",    _A_LED,   C_LEVEL,    5},
    {"fan",    _A_FAN,   C_LEVEL,    30},
    {"motor",  _A_MOTOR, C_LEVEL,    5},
    {"servo",  _A_SERVO, C_POSITION, 3},
    {"strip",  _A_STRIP, C_COLOR,    5},
};

static constexpr uint8_t SENSOR_COUNT   = sizeof(SENSORS) / sizeof(SENSORS[0]);
static constexpr uint8_t ACTUATOR_COUNT = sizeof(ACTUATORS) / sizeof(ACTUATORS[0]);

// Panel list groups (optgroup), in the order of the group/cap indexes
static const char _G0[] PROGMEM = L_G_TEMP_HUM;
static const char _G1[] PROGMEM = L_G_MOTION;
static const char _G2[] PROGMEM = L_G_DISTANCE;
static const char _G3[] PROGMEM = L_G_ONOFF;
static const char _G4[] PROGMEM = L_G_LEVEL;
static const char _G5[] PROGMEM = L_G_POSITION;
static const char _G6[] PROGMEM = L_COLOR;
static const char* const SENSOR_GROUPS[]   = {_G0, _G1, _G2};
static const char* const ACTUATOR_GROUPS[] = {_G3, _G4, _G5, _G6}; // by Cap - 1

// Capability name in the API (stable, like the slugs)
static const char* const CAP_NAMES[] = {"none", "onoff", "level", "position", "color"};

// The four free ESP-01 pins: GPIO0, TX (1), GPIO2 and RX (3)
inline bool allowedPin(int pin) {
    return pin >= 0 && pin <= 3;
}

// Position of the slug in the table; -1 if empty or unknown
inline int8_t sensorIndex(const char* slug) {
    for (uint8_t i = 0; i < SENSOR_COUNT; i++)
        if (strcmp(slug, SENSORS[i].slug) == 0) return (int8_t)i;
    return -1;
}

inline int8_t actuatorIndex(const char* slug) {
    for (uint8_t i = 0; i < ACTUATOR_COUNT; i++)
        if (strcmp(slug, ACTUATORS[i].slug) == 0) return (int8_t)i;
    return -1;
}

// Configuration v1 stored the position in the old tables (16 sensors, 8
// modules). Rain, flame, gas, soil, light, sound and float become "digital".
static const char* const V1_SENSORS[] = {
    "digital", "dht11", "dht22", "ds18b20", "pulses", "pir", "contact",
    "digital", "digital", "digital", "digital", "digital", "digital",
    "digital", "button", "distance",
};
static const char* const V1_ACTUATORS[] = {
    "relay", "servo", "led", "buzzer", "pump", "valve", "fan", "motor",
};

inline const char* v1Sensor(uint8_t i) {
    return i < sizeof(V1_SENSORS) / sizeof(V1_SENSORS[0]) ? V1_SENSORS[i] : "digital";
}

inline const char* v1Actuator(uint8_t i) {
    return i < sizeof(V1_ACTUATORS) / sizeof(V1_ACTUATORS[0]) ? V1_ACTUATORS[i] : "relay";
}

} // namespace iot
