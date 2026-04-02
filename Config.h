#pragma once
#include <Arduino.h>

#define MAX_RECORDS 5

struct Config {
    String  webusr;
    String  webpss;
    String  ssid;
    String  pass;

    char    cf_token[48];
    char    cf_zone[40];
    char    cf_records[MAX_RECORDS][40];
    uint8_t cf_record_count;
    char    cf_host[32];

    char    ntfy_topic[64];

    // Estado em runtime — escrito pelos serviços, lido pelo WebService
    char          publicIP[16];
    unsigned long lastDnsUpdate;  // millis() da última atualização bem-sucedida
};
