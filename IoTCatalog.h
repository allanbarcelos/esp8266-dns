// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// Catálogo de sensores e atuadores. A configuração grava o identificador
// (slug), nunca a posição na tabela: a ordem e os nomes de exibição podem
// mudar à vontade. Um slug publicado nunca é renomeado nem reaproveitado.
//
// Módulo novo que usa uma capacidade existente é só uma linha nas tabelas:
// painel, comandos, API e ntfy passam a funcionar sem outro código.
// Capacidade nova pede driver e controle de tela (IoTService/WebService).
namespace iot {

// Como o sensor é lido
enum Kind : uint8_t { K_NONE, K_DIGITAL, K_DHT11, K_DHT22, K_DS18, K_RANGE, K_PULSE };

// O que o atuador faz: define o controle na tela e os comandos aceitos
enum Cap : uint8_t { C_NONE, C_ONOFF, C_LEVEL, C_POSITION, C_COLOR };

struct SensorDef {
    const char* slug;   // RAM: vai para JSON e comparações
    PGM_P       label;  // flash: só aparece no painel
    uint8_t     kind;
    uint8_t     group;  // grupo na lista do painel
    PGM_P       on;     // digital: nome do estado ativo / inativo
    PGM_P       off;
};

struct ActuatorDef {
    const char* slug;
    PGM_P       label;
    uint8_t     cap;
    uint8_t     pulse;  // duração sugerida do pulso no painel (s)
};

static const char _S_DHT22[]  PROGMEM = "Temperatura e umidade (DHT22)";
static const char _S_DHT11[]  PROGMEM = "Temperatura e umidade (DHT11)";
static const char _S_DS18[]   PROGMEM = "Temperatura (DS18B20)";
static const char _S_PIR[]    PROGMEM = "Presenca (PIR)";
static const char _S_DOOR[]   PROGMEM = "Porta ou janela (reed)";
static const char _S_BTN[]    PROGMEM = "Botao ou interruptor";
static const char _S_DIG[]    PROGMEM = "Digital (chuva, gas, chama, solo, boia, som)";
static const char _S_RANGE[]  PROGMEM = "Distancia (HC-SR04)";
static const char _S_PULSE[]  PROGMEM = "Vazao ou pulsos (YF-S201)";

static const char _T_PRES[]   PROGMEM = "presenca";
static const char _T_FREE[]   PROGMEM = "livre";
static const char _T_OPEN[]   PROGMEM = "aberta";
static const char _T_SHUT[]   PROGMEM = "fechada";
static const char _T_PRESS[]  PROGMEM = "pressionado";
static const char _T_REL[]    PROGMEM = "solto";
static const char _T_ACT[]    PROGMEM = "ativo";
static const char _T_IDLE[]   PROGMEM = "inativo";

static const char _A_RELAY[]  PROGMEM = "Rele";
static const char _A_PUMP[]   PROGMEM = "Bomba";
static const char _A_VALVE[]  PROGMEM = "Valvula solenoide";
static const char _A_LOCK[]   PROGMEM = "Fechadura solenoide";
static const char _A_BUZZ[]   PROGMEM = "Buzzer";
static const char _A_LED[]    PROGMEM = "LED com brilho";
static const char _A_FAN[]    PROGMEM = "Ventilador";
static const char _A_MOTOR[]  PROGMEM = "Motor DC";
static const char _A_SERVO[]  PROGMEM = "Servo";
static const char _A_STRIP[]  PROGMEM = "Fita de LED (WS2812)";

static const SensorDef SENSORS[] = {
    {"dht22",     _S_DHT22, K_DHT22,   0, nullptr, nullptr},
    {"dht11",     _S_DHT11, K_DHT11,   0, nullptr, nullptr},
    {"ds18b20",   _S_DS18,  K_DS18,    0, nullptr, nullptr},
    {"pir",       _S_PIR,   K_DIGITAL, 1, _T_PRES,  _T_FREE},
    {"contato",   _S_DOOR,  K_DIGITAL, 1, _T_OPEN,  _T_SHUT},
    {"botao",     _S_BTN,   K_DIGITAL, 1, _T_PRESS, _T_REL},
    {"digital",   _S_DIG,   K_DIGITAL, 1, _T_ACT,   _T_IDLE},
    {"distancia", _S_RANGE, K_RANGE,   2, nullptr, nullptr},
    {"pulsos",    _S_PULSE, K_PULSE,   2, nullptr, nullptr},
};

static const ActuatorDef ACTUATORS[] = {
    {"rele",       _A_RELAY, C_ONOFF,    1},
    {"bomba",      _A_PUMP,  C_ONOFF,    10},
    {"valvula",    _A_VALVE, C_ONOFF,    10},
    {"fechadura",  _A_LOCK,  C_ONOFF,    3},
    {"buzzer",     _A_BUZZ,  C_ONOFF,    1},
    {"led",        _A_LED,   C_LEVEL,    5},
    {"ventilador", _A_FAN,   C_LEVEL,    30},
    {"motor",      _A_MOTOR, C_LEVEL,    5},
    {"servo",      _A_SERVO, C_POSITION, 3},
    {"fita",       _A_STRIP, C_COLOR,    5},
};

static constexpr uint8_t SENSOR_COUNT   = sizeof(SENSORS) / sizeof(SENSORS[0]);
static constexpr uint8_t ACTUATOR_COUNT = sizeof(ACTUATORS) / sizeof(ACTUATORS[0]);

// Grupos da lista do painel (optgroup), na ordem dos índices de group/cap
static const char _G0[] PROGMEM = "Temperatura e umidade";
static const char _G1[] PROGMEM = "Presenca e contato";
static const char _G2[] PROGMEM = "Distancia e vazao";
static const char _G3[] PROGMEM = "Liga e desliga";
static const char _G4[] PROGMEM = "Intensidade (PWM)";
static const char _G5[] PROGMEM = "Posicao";
static const char _G6[] PROGMEM = "Cor";
static const char* const SENSOR_GROUPS[]   = {_G0, _G1, _G2};
static const char* const ACTUATOR_GROUPS[] = {_G3, _G4, _G5, _G6}; // por Cap - 1

// Nome da capacidade na API (estável, como os slugs)
static const char* const CAP_NAMES[] = {"none", "onoff", "level", "position", "color"};

// Os quatro pinos livres do ESP-01: GPIO0, TX (1), GPIO2 e RX (3)
inline bool allowedPin(int pin) {
    return pin >= 0 && pin <= 3;
}

// Posição do slug na tabela; -1 se vazio ou desconhecido
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

// Configuração v1 gravava a posição nas tabelas antigas (16 sensores, 8
// módulos). Chuva, chama, gás, solo, luz, som e boia viram "digital".
static const char* const V1_SENSORS[] = {
    "digital", "dht11", "dht22", "ds18b20", "pulsos", "pir", "contato",
    "digital", "digital", "digital", "digital", "digital", "digital",
    "digital", "botao", "distancia",
};
static const char* const V1_ACTUATORS[] = {
    "rele", "servo", "led", "buzzer", "bomba", "valvula", "ventilador", "motor",
};

inline const char* v1Sensor(uint8_t i) {
    return i < sizeof(V1_SENSORS) / sizeof(V1_SENSORS[0]) ? V1_SENSORS[i] : "digital";
}

inline const char* v1Actuator(uint8_t i) {
    return i < sizeof(V1_ACTUATORS) / sizeof(V1_ACTUATORS[0]) ? V1_ACTUATORS[i] : "rele";
}

} // namespace iot
