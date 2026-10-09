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

    // Estado em runtime — escrito pelos serviços, lido pelo WebService
    char          publicIP[16]  = {};
    unsigned long lastDnsUpdate = 0;  // millis() da última sincronização bem-sucedida
};
