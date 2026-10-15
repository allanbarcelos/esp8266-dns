// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include "Version.h"
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"
#include "CloudflareDNS.h"
#include "CloudflareSetup.h"
#include "OTAService.h"
#include "IoTService.h"
#include "NtfyNotifier.h"
#include "MqttService.h"
#include "PowerService.h"
#include "WebService.h"
#include "RelayService.h"

// Composition of every service. Members are declared in layers: each one
// depends only on those declared before it.
class App {
public:
    // Layer 0: no dependencies
    Config config;
    Logger logger;
    Clock  clock;

    // Layer 1: depend on Config + Logger
    ConfigStore configStore{config, logger};
    WiFiService wifi{config, logger};

    // Layer 2: depend on WiFi and the above
    PublicIPResolver resolver{config, logger, clock};
    CloudflareDNS    dns{config, logger, wifi, clock};
    OTAService       ota{logger, wifi};
    IoTService       iot{config, logger, &clock};
    NtfyNotifier     ntfy{config, logger, clock, &iot};
    WiFiClient       mqttNet;
    MqttService      mqtt{config, logger, iot, mqttNet};
    PowerService     power{config, logger};
    CloudflareSetup  cfsetup{config, logger, configStore, clock, &dns};

    // Layer 3: web panel
    WebService web{config, logger, configStore, wifi, ota, iot, &ntfy, &mqtt, &power, &cfsetup};

    // Layer 4: remote access, through the panel's server
    RelayService relay{config, logger, clock, web.server(), &iot, &ntfy};

    void begin() {
        Serial.begin(115200);
        Serial.printf("\n=== ESP8266 DNS Updater %s ===\n", firmware_version);

        configStore.begin();
        configStore.load();

        wifi.registerEvents();
        wifi.begin();
        clock.begin();

        ota.setAutomatic(config.ota_auto);
        ota.begin();
        logger.log(PSTR("OTA: reserve %u KB, free %u KB"),
                   FlashLayout::OTA_RESERVE / 1024u,
                   ESP.getFreeSketchSpace() / 1024u);
        if (ota.slim()) {
            logger.log(PSTR("OTA: recovery mode"));
        } else {
            web.setRelay(&relay);
            web.begin();
            iot.begin();
            power.begin();
        }

        // getResetReason survives the reboot and says why the previous boot died.
        logger.log(PSTR("Reset: %s"), ESP.getResetReason().c_str());
        logger.log(PSTR("System started %s"), firmware_version);
    }

    void tick() {
        unsigned long now = millis();

        if (!ota.slim()) web.tick();
        wifi.tick(now);
        wifi.tickRestart(now);
        clock.tick(now);

        // One TLS session at a time: the ntfy command stream and the relay
        // release theirs before the IP check, DNS, OTA or an ntfy post use
        // the network in this tick. While the relay is up or about to
        // connect, it has the session; its Worker brings the ntfy commands
        // until it has been down a while, and then the stream takes over.
        // After an update with another relay protocol, the Worker goes up again
        if (!ota.slim()) cfsetup.redeploy(now);
        bool net = !ota.slim() && (ota.wantsNet(now) || resolver.due(now) || dns.due(now) || cfsetup.due());
        relay.hold(ota.slim() || ota.busy() || net || ntfy.postDue(now));
        // MQTT is plain TCP, no TLS, but its buffers are heap the GitHub
        // session needs: it lets go while OTA checks or downloads
        mqtt.hold(ota.slim() || ota.busy());
        if (!ota.slim()) {
            ntfy.hold(net || relay.up() || relay.due(now));
            ntfy.relayed(relay.carries(now));
        }

        // A new IP reaches DNS in the same tick
        if (!ota.slim() && !ota.busy()) {
            resolver.tick(now);
            dns.tick(now);
            cfsetup.tick(now);
            if (cfsetup.takeDeployed()) relay.reconnect();
        }
        ota.tick(now);
        if (!ota.slim() && !ota.inProgress()) {
            iot.tick(now);
            power.tick(now);
        }
        // ntfy opens another TLS session: waits for OTA to release its own (heap)
        if (!ota.slim() && !ota.busy()) ntfy.tick(now);
        relay.tick(now);
        mqtt.tick(now);
    }
};
