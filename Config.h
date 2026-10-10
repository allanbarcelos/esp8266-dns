#pragma once
#include <Arduino.h>

#define MAX_RECORDS 5

// Rede de configuração (modo AP). A senha pode ser trocada no build com
// -DAP_PASSWORD=\"...\"; o WPA2 exige no mínimo 8 caracteres.
#ifndef AP_SSID
#define AP_SSID "ESP_Config"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "esp8266dns"
#endif

#define MIN_PANEL_PASSWORD_LEN 8

struct Config {
    String  webusr;
    String  webpss;
    String  ssid;
    String  pass;

    char    cf_token[48]              = {};
    char    cf_zone[40]               = {};
    char    cf_records[MAX_RECORDS][40] = {};
    uint8_t cf_record_count           = 0;
    char    cf_host[64]               = {};  // apenas exibição no painel

    char    ntfy_topic[64]            = {};
    bool    ota_auto                  = true; // ausência no JSON mantém o automático ligado

    // IoT. Pino -1 desliga a função. Ausência no JSON deixa tudo desligado.
    int8_t  iot_sensor_pin            = -1;
    int8_t  iot_module_pin            = -1;
    bool    iot_sensor_high           = true;  // nível do sensor que liga o módulo
    bool    iot_module_high           = false; // nível do pino que liga o módulo
    bool    iot_notify                = false;
    // 0 digital, 1 DHT11, 2 DHT22, 3 DS18B20, 4 pulsos. Ausência = digital.
    uint8_t iot_sensor_type           = 0;
    // 0 nível fixo, 1 servo. Ausência = nível.
    uint8_t iot_module_type           = 0;
    uint8_t iot_metric                = 0;     // DHT: 0 umidade, 1 temperatura
    bool    iot_above                 = false; // true: liga acima do limite
    int16_t iot_threshold             = 0;
    uint8_t iot_margin                = 0;
    uint8_t iot_angle_on              = 90;
    uint8_t iot_angle_off             = 0;

    // Estado em runtime — escrito pelos serviços, lido pelo WebService
    char          publicIP[16]  = {};
    unsigned long lastDnsUpdate = 0;  // millis() da última sincronização bem-sucedida
};
