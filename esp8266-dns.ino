#include <LittleFS.h>
#include "Config.h"
#include "Logger.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"
#include "CloudflareDNS.h"
#include "OTAService.h"
#include "NtfyNotifier.h"
#include "WebService.h"

#ifndef firmware_version
#define firmware_version "dev"
#endif

// Camada 0 — sem dependências
Config config;
Logger logger;

// Camada 1 — dependem de Config + Logger
ConfigStore     configStore(config, logger);
WiFiService     wifi(config, logger);

// Camada 2 — dependem de WiFi + acima
PublicIPResolver resolver(logger);
CloudflareDNS    dns(config, logger, wifi, resolver);
OTAService       ota(logger, wifi);
NtfyNotifier     ntfy(config, logger);

// Camada 3 — agrega tudo
WebService webService(config, logger, configStore, wifi, dns, ota, ntfy);

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.printf("\n=== ESP8266 DNS Updater v%s ===\n", firmware_version);

    memset(config.cf_token,    0, sizeof(config.cf_token));
    memset(config.cf_zone,     0, sizeof(config.cf_zone));
    memset(config.cf_records,  0, sizeof(config.cf_records));
    memset(config.ntfy_topic,  0, sizeof(config.ntfy_topic));
    memset(config.publicIP,    0, sizeof(config.publicIP));
    config.cf_record_count = 0;
    config.lastDnsUpdate   = 0;

    configStore.begin();
    configStore.load();

    wifi.registerEvents();
    wifi.begin();

    webService.begin();

    logger.log("Sistema inicializado v%s", firmware_version);
}

void loop() {
    unsigned long now = millis();

    webService.tick();
    wifi.tick(now);
    wifi.tickRestart(now);

    dns.tick(now);
    ota.tick(now);
    ntfy.tick(now);
}
