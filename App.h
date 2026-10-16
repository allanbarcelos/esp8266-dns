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
#include "NetArbiter.h"

// Composition of every service. Members are declared in layers: each one
// depends only on those declared before it.
class App {
public:
    // Layer 0: no dependencies
    Config config;
    Logger logger;
    Clock  clock;
    NetArbiter net;   // who has the network (one TLS session)

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
            web.setNet(&net);
            web.begin();
            iot.begin();
            power.begin();
        }

        // getResetReason survives the reboot and says why the previous boot died.
        logger.log(PSTR("Reset: %s"), ESP.getResetReason().c_str());
        char crash[NetArbiter::CRASH_LEN];
        if (net.lastCrash(crash)) logger.log(PSTR("Crashed in: %s"), crash);
        net.at(NetArbiter::NONE);
        logger.log(PSTR("System started %s"), firmware_version);
    }

    void tick() {
        unsigned long now = millis();

        net.at(NetArbiter::WEB);
        if (!ota.slim()) web.tick();
        net.at(NetArbiter::WIFI);
        wifi.tick(now);
        wifi.tickRestart(now);
        clock.tick(now);

        // One TLS session fits: whoever needs the network says so, the
        // arbiter gives the turn by priority, and everyone else closes theirs
        // before it runs. The relay's Worker brings the ntfy commands while it
        // is up or was until moments ago; then the stream takes them over.
        bool slim = ota.slim();
        if (!slim) cfsetup.redeploy(now);   // after an update with another relay protocol
        if (ota.wantsNet(now)) net.want(NetArbiter::OTA);
        if (!slim) {
            if (resolver.due(now)) net.want(NetArbiter::IP);
            if (dns.due(now)) net.want(NetArbiter::DNS);
            if (cfsetup.due()) net.want(NetArbiter::SETUP);
            if (ntfy.postDue(now)) net.want(NetArbiter::POST);
            if (relay.wantsNet(now)) net.want(NetArbiter::RELAY);
            if (ntfy.wantsCommands() && !relay.carries(now)) net.want(NetArbiter::STREAM);
        }
        NetArbiter::Who who = net.grant();
        relay.hold(who != NetArbiter::RELAY);
        ntfy.hold(who != NetArbiter::STREAM);   // a post closes the stream first anyway
        ntfy.relayed(relay.carries(now));
        bool go = who != NetArbiter::NONE && net.ready(now, _need(who), _running(who));
        // MQTT is plain TCP, but its buffers are heap the GitHub session, or a
        // turn waiting for memory, needs
        mqtt.hold(slim || ota.busy() || net.starved());

        net.at(who);
        if (go) {
            switch (who) {
                case NetArbiter::IP:    resolver.tick(now); break;
                case NetArbiter::DNS:   dns.tick(now); break;
                case NetArbiter::SETUP:
                    cfsetup.tick(now);
                    if (cfsetup.takeDeployed()) relay.reconnect();
                    break;
                case NetArbiter::POST:
                case NetArbiter::STREAM: ntfy.tick(now); break;
                default: break;   // OTA and the relay, below
            }
        }
        net.at(NetArbiter::OTA);
        if (who != NetArbiter::OTA || go) ota.tick(now);
        net.at(NetArbiter::IOT);
        if (!slim && !ota.inProgress()) {
            iot.tick(now);
            power.tick(now);
        }
        net.at(NetArbiter::RELAY);
        if (who != NetArbiter::RELAY || go) relay.tick(now);
        net.at(NetArbiter::MQTT);
        mqtt.tick(now);
        net.at(NetArbiter::NONE);

        // A TLS handshake blocks the loop for seconds: the slowest one is logged
        unsigned long before = net.worst();
        net.loop(millis() - now);
        if (net.worst() > before)
            logger.log(PSTR("Slowest loop so far: %lu ms (%s)"), net.worst(), NetArbiter::name(net.worstWho()));
    }

private:
    // The heap a new session needs: GitHub's 16 KB buffer, or the others' 4 KB
    static NetArbiter::Need _need(NetArbiter::Who who) {
        if (who == NetArbiter::OTA) return {OTAService::GITHUB_HEAP, OTAService::GITHUB_BLOCK};
        return {PublicIPResolver::MIN_TLS_HEAP, PublicIPResolver::MIN_TLS_BLOCK};
    }

    // Its session is open already: no wait for the heap it took
    bool _running(NetArbiter::Who who) const {
        return (who == NetArbiter::OTA && ota.online()) || (who == NetArbiter::RELAY && relay.up()) ||
               (who == NetArbiter::STREAM && ntfy.streaming());
    }
};
