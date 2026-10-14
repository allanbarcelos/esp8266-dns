// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// Catalog of sensors and actuators. The configuration stores the identifier
// (slug), never the position in the table: the order and the display names
// can change freely. A published slug is never renamed or reused.
//
// A new module that uses an existing capability is a single line in the
// tables: panel, commands, API and ntfy work with no other code. A new
// capability needs a driver and a panel control (IoTService/WebService).
namespace iot {

// How the sensor is read
enum Kind : uint8_t { K_NONE, K_DIGITAL, K_DHT11, K_DHT22, K_DS18, K_RANGE, K_PULSE, K_SHT3X };

// Temperature and humidity together (DHT or SHT3x)
inline bool tempAndHumidity(uint8_t k) { return k == K_DHT11 || k == K_DHT22 || k == K_SHT3X; }

// What the actuator does: sets the panel control and the accepted commands
enum Cap : uint8_t { C_NONE, C_ONOFF, C_LEVEL, C_POSITION, C_COLOR };

struct SensorDef {
    const char* slug;   // RAM: goes to JSON and comparisons
    PGM_P       label;  // flash: only shown on the panel
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

static const char _S_DHT22[]  PROGMEM = "Temperature and humidity (DHT22)";
static const char _S_DHT11[]  PROGMEM = "Temperature and humidity (DHT11)";
static const char _S_DS18[]   PROGMEM = "Temperature (DS18B20)";
static const char _S_SHT3X[]  PROGMEM = "Temperature and humidity (SHT30/SHT31, I2C)";
static const char _S_PIR[]    PROGMEM = "Motion (PIR)";
static const char _S_DOOR[]   PROGMEM = "Door or window (reed)";
static const char _S_BTN[]    PROGMEM = "Button or switch";
static const char _S_DIG[]    PROGMEM = "Digital (rain, gas, flame, soil, float, sound)";
static const char _S_RANGE[]  PROGMEM = "Distance (HC-SR04)";
static const char _S_PULSE[]  PROGMEM = "Flow or pulses (YF-S201)";

static const char _T_MOTION[] PROGMEM = "motion";
static const char _T_CLEAR[]  PROGMEM = "clear";
static const char _T_OPEN[]   PROGMEM = "open";
static const char _T_SHUT[]   PROGMEM = "closed";
static const char _T_PRESS[]  PROGMEM = "pressed";
static const char _T_REL[]    PROGMEM = "released";
static const char _T_ACT[]    PROGMEM = "active";
static const char _T_IDLE[]   PROGMEM = "idle";

static const char _A_RELAY[]  PROGMEM = "Relay";
static const char _A_PUMP[]   PROGMEM = "Pump";
static const char _A_VALVE[]  PROGMEM = "Solenoid valve";
static const char _A_LOCK[]   PROGMEM = "Solenoid lock";
static const char _A_BUZZ[]   PROGMEM = "Buzzer";
static const char _A_LED[]    PROGMEM = "Dimmable LED";
static const char _A_FAN[]    PROGMEM = "Fan";
static const char _A_MOTOR[]  PROGMEM = "DC motor";
static const char _A_SERVO[]  PROGMEM = "Servo";
static const char _A_STRIP[]  PROGMEM = "LED strip (WS2812)";

static const SensorDef SENSORS[] = {
    {"dht22",    _S_DHT22, K_DHT22,   0, nullptr,   nullptr},
    {"dht11",    _S_DHT11, K_DHT11,   0, nullptr,   nullptr},
    {"ds18b20",  _S_DS18,  K_DS18,    0, nullptr,   nullptr},
    {"sht3x",    _S_SHT3X, K_SHT3X,   0, nullptr,   nullptr},
    {"pir",      _S_PIR,   K_DIGITAL, 1, _T_MOTION, _T_CLEAR},
    {"contact",  _S_DOOR,  K_DIGITAL, 1, _T_OPEN,   _T_SHUT},
    {"button",   _S_BTN,   K_DIGITAL, 1, _T_PRESS,  _T_REL},
    {"digital",  _S_DIG,   K_DIGITAL, 1, _T_ACT,    _T_IDLE},
    {"distance", _S_RANGE, K_RANGE,   2, nullptr,   nullptr},
    {"pulses",   _S_PULSE, K_PULSE,   2, nullptr,   nullptr},
};

static const ActuatorDef ACTUATORS[] = {
    {"relay",  _A_RELAY, C_ONOFF,    1},
    {"pump",   _A_PUMP,  C_ONOFF,    10},
    {"valve",  _A_VALVE, C_ONOFF,    10},
    {"lock",   _A_LOCK,  C_ONOFF,    3},
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
static const char _G0[] PROGMEM = "Temperature and humidity";
static const char _G1[] PROGMEM = "Motion and contact";
static const char _G2[] PROGMEM = "Distance and flow";
static const char _G3[] PROGMEM = "On and off";
static const char _G4[] PROGMEM = "Level (PWM)";
static const char _G5[] PROGMEM = "Position";
static const char _G6[] PROGMEM = "Color";
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
