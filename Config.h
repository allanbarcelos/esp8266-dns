// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// Rede de configuração (modo AP). A senha é sorteada por aparelho no
// primeiro boot (WiFiService::apPassword); -DAP_PASSWORD=\"...\" no build
// fixa uma senha (o WPA2 exige no mínimo 8 caracteres).
#ifndef AP_SSID
#define AP_SSID "ESP_Config"
#endif

#define MIN_PANEL_PASSWORD_LEN 8

// IoT: um sensor e um atuador, cada um opcional. Tipos pelo identificador
// do catálogo (IoTCatalog.h); identificador vazio ou pino -1 desliga a parte.
struct IoTConfig {
    char     sensor[12]    = {};
    int8_t   sensorPin     = -1;
    bool     sensorHigh    = true;   // digital: nível que significa "ativo"
    char     onText[16]    = {};     // digital: nome dos estados ("" = do tipo)
    char     offText[16]   = {};
    uint8_t  metric        = 0;      // DHT: a regra usa 0 umidade, 1 temperatura
    bool     notify        = false;  // leitura e estado vão ao tópico ntfy

    char     actuator[12]  = {};
    int8_t   actuatorPin   = -1;
    bool     actuatorHigh  = false;  // liga/desliga: nível do pino que liga
    uint8_t  angleOn       = 90;     // posição: aberto / fechado
    uint8_t  angleOff      = 0;
    uint8_t  dutyOn        = 100;    // intensidade ao ligar / quando desligado
    uint8_t  dutyOff       = 0;
    uint8_t  leds          = 8;      // cor: LEDs na fita
    uint16_t maxOn         = 0;      // minutos ligado até desligar sozinho; 0 = sem limite
    bool     restore       = false;  // ao religar: último comando (senão, desligado)

    // Regra (sensor e atuador): o sensor comanda o atuador no modo automático
    bool     rule          = true;
    bool     above         = false;  // numérico: liga acima do limite (senão, abaixo)
    int16_t  threshold     = 0;
    uint8_t  margin        = 0;      // histerese para desligar
    bool     toggle        = false;  // digital: cada acionamento inverte o atuador
    bool     linear        = false;  // intensidade proporcional entre os limites
    int16_t  thresholdHi   = 0;
};

struct Config {
    String  webusr;
    String  webpss;
    String  ssid;
    String  pass;

    char    cf_token[48]              = {};
    char    cf_zone[40]               = {};
    char    cf_record[40]             = {};  // ID do registro A atualizado
    char    cf_host[64]               = {};  // apenas exibição no painel

    char    ntfy_topic[64]            = {};
    char    ntfy_cmd[64]              = {};   // tópico de comandos; "" desliga
    char    ntfy_token[48]            = {};   // token de acesso do ntfy (opcional)
    bool    ota_auto                  = true; // ausência no JSON mantém o automático ligado
    char    api_token[33]             = {};   // API local (Bearer); gerado com o IoT
    char    mqtt_host[64]             = {};   // "host" ou "host:porta"; "" desliga o MQTT
    char    mqtt_user[32]             = {};
    char    mqtt_pass[64]             = {};
    char    dev_name[32]              = {};   // nome no painel e no Home Assistant; "" = padrão
    char    hostname[33]              = {};   // nome no DHCP do roteador; "" = o do core (ESP-xxxxxx)

    IoTConfig iot;

    // Estado em runtime — escrito pelos serviços, lido pelo WebService
    char          publicIP[16]  = {};   // PublicIPResolver, a cada 5 min
    char          publicHost[64] = {};  // nome reverso (PTR) do IP público
    unsigned long ipChanged     = 0;    // millis() da última mudança do IP público
    unsigned long ipChecked     = 0;    // millis() da última consulta que deu certo
    unsigned long lastDnsUpdate = 0;    // millis() da última sincronização bem-sucedida
};
