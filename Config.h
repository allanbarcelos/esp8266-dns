// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// Setup network (AP mode). The password is drawn per device on the first
// boot (WiFiService::apPassword); -DAP_PASSWORD=\"...\" at build time sets
// a fixed one (WPA2 needs at least 8 characters).
#ifndef AP_SSID
#define AP_SSID "ESP_Config"
#endif

#define MIN_PANEL_PASSWORD_LEN 8

// IoT: one sensor and one actuator, each optional. Types by catalog
// identifier (IoTCatalog.h); an empty identifier or pin -1 turns the part off.
struct IoTConfig {
    char     sensor[12]    = {};
    int8_t   sensorPin     = -1;
    bool     sensorHigh    = true;   // digital: level that means "active"
    char     onText[16]    = {};     // digital: state names ("" = the type's)
    char     offText[16]   = {};
    uint8_t  metric        = 0;      // DHT/SHT3x: the rule uses 0 humidity, 1 temperature
    bool     notify        = false;  // readings and state go to the ntfy topic

    char     actuator[12]  = {};
    int8_t   actuatorPin   = -1;
    bool     actuatorHigh  = false;  // on/off: pin level that turns it on
    uint8_t  angleOn       = 90;     // position: open / closed
    uint8_t  angleOff      = 0;
    uint8_t  dutyOn        = 100;    // level when on / when off
    uint8_t  dutyOff       = 0;
    uint8_t  leds          = 8;      // color: LEDs on the strip
    uint16_t maxOn         = 0;      // minutes on before turning off by itself; 0 = no limit
    bool     restore       = false;  // on power-up: last command (otherwise off)

    // Rule (sensor and actuator): the sensor drives the actuator in automatic mode
    bool     rule          = true;
    bool     above         = false;  // numeric: on above the limit (otherwise below)
    int16_t  threshold     = 0;
    uint8_t  margin        = 0;      // hysteresis to turn off
    bool     toggle        = false;  // digital: each activation flips the actuator
    bool     linear        = false;  // level proportional between the limits
    int16_t  thresholdHi   = 0;
    // Time window (local time): outside it, automatic mode stays off
    bool     window        = false;
    uint16_t winStart      = 0;      // minute of the day it opens
    uint16_t winEnd        = 0;      // minute of the day it closes (before the start: wraps past midnight)
};

struct Config {
    String  webusr;
    String  webpss;          // Password.h: the salted hash ("" = none yet)
    uint8_t totp[20] = {};   // authenticator secret; all zeros = not enrolled
    uint32_t totpStep = 0;   // last authenticator step accepted (a code works once)
    uint32_t totpBrowserStep = 0;  // the same, for codes checked against the browser's clock
    String  ssid;
    String  pass;

    char    cf_token[48]              = {};
    char    cf_zone[40]               = {};
    char    cf_record[40]             = {};  // ID of the A record to update
    char    cf_host[64]               = {};  // the record's name
    char    cf_account[33]            = {};  // the account the token reaches (CloudflareSetup)

    // Remote access relay (RelayService.h): the Worker's address, the secret
    // shared with it and the protocol version it was deployed with
    char    relay_host[80]            = {};   // "" = off
    char    relay_name[33]            = {};   // the Worker's name, also the first label of the address
    char    relay_zone[33]            = {};   // the zone of the address; "" = workers.dev
    char    relay_secret[65]          = {};   // 64 hex
    uint8_t relay_ver                 = 0;

    char    ntfy_topic[64]            = {};
    char    ntfy_cmd[64]              = {};   // command topic; "" turns it off
    char    ntfy_token[48]            = {};   // ntfy access token (optional)
    uint8_t ntfy_poll                 = 5;    // seconds between the relay Worker's checks of the command topic
    bool    ota_auto                  = true; // missing from the JSON keeps automatic updates on
    char    api_token[33]             = {};   // local API (Bearer); generated with the IoT setup
    char    mqtt_host[64]             = {};   // "host" or "host:port"; "" turns MQTT off
    char    mqtt_user[32]             = {};
    char    mqtt_pass[64]             = {};
    char    dev_name[32]              = {};   // name on the panel and in Home Assistant; "" = default
    char    hostname[33]              = {};   // name in the router's DHCP; "" = the core's (ESP-xxxxxx)
    char    web_hosts[64]             = {};   // other names the panel answers to (tunnel), comma-separated
    int16_t tz                        = 0;    // time zone: minutes from UTC (local time)
    int8_t  bat_sda                   = -1;   // battery gauge (MAX17048): SDA; -1 = no gauge
    char    lang[3]                   = {};   // panel language (en, pt, es, fr); "" = the browser's

    IoTConfig iot;

    // Runtime state, written by the services and read by WebService
    char          publicIP[16]  = {};   // PublicIPResolver, every 5 min
    char          publicHost[64] = {};  // reverse name (PTR) of the public IP
    unsigned long ipChanged     = 0;    // millis() of the last public IP change
    unsigned long ipChecked     = 0;    // millis() of the last successful check
    unsigned long lastDnsUpdate = 0;    // millis() of the last successful sync
};
