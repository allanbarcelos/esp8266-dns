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
#include "OTAService.h"
#include "NtfyNotifier.h"
#include "WebService.h"

// Composição de todos os serviços. Os membros são declarados em camadas:
// cada um depende apenas dos declarados antes dele.
class App {
public:
    // Camada 0 — sem dependências
    Config config;
    Logger logger;
    Clock  clock;

    // Camada 1 — dependem de Config + Logger
    ConfigStore configStore{config, logger};
    WiFiService wifi{config, logger};

    // Camada 2 — dependem de WiFi + acima
    PublicIPResolver resolver{logger};
    CloudflareDNS    dns{config, logger, wifi, resolver, clock};
    OTAService       ota{logger, wifi};
    NtfyNotifier     ntfy{config, logger};

    // Camada 3 — painel web
    WebService web{config, logger, configStore, wifi, ota};

    void begin() {
        Serial.begin(115200);
        delay(1000);
        Serial.printf("\n=== ESP8266 DNS Updater %s ===\n", firmware_version);

        configStore.begin();
        configStore.load();

        wifi.registerEvents();
        wifi.begin();
        clock.begin();

        web.begin();

        logger.log("Sistema inicializado %s", firmware_version);
    }

    void tick() {
        unsigned long now = millis();

        web.tick();
        wifi.tick(now);
        wifi.tickRestart(now);

        dns.tick(now);
        ota.tick(now);
        ntfy.tick(now);
    }
};
