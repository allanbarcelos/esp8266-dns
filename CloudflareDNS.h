#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Logger.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"

class CloudflareDNS {
public:
    CloudflareDNS(Config& cfg, Logger& log,
                  WiFiService& wifi, PublicIPResolver& resolver)
        : _cfg(cfg), _log(log), _wifi(wifi), _resolver(resolver),
          _lastCheck(0) {}

    void tick(unsigned long now) {
        if (strlen(_cfg.cf_token) == 0) return;
        if (now - _lastCheck < CHECK_INTERVAL) return;
        _lastCheck = now;
        _update();
    }

    unsigned long lastUpdateMs() const { return _cfg.lastDnsUpdate; }

private:
    Config&           _cfg;
    Logger&           _log;
    WiFiService&      _wifi;
    PublicIPResolver& _resolver;
    unsigned long     _lastCheck;

    static const unsigned long CHECK_INTERVAL = 60000UL;

    void _update() {
        if (!_resolver.resolve(_cfg.publicIP, sizeof(_cfg.publicIP))) {
            _log.log("Falha ao obter IP publico");
            return;
        }

        char dnsIP[16] = {0};
        if (!_getCurrentDNSIP(dnsIP, sizeof(dnsIP))) return;

        _log.log("PublicIP: %s", _cfg.publicIP);
        _log.log("CurrentDNSIP: %s", dnsIP);

        if (strcmp(_cfg.publicIP, dnsIP) != 0) {
            _log.log("Atualizando DNS...");
            _patchRecords(_cfg.publicIP);
        } else {
            _log.log("DNS ja atualizado.");
        }
    }

    bool _getCurrentDNSIP(char* out, size_t len) {
        IPAddress resolved;
        if (!WiFi.hostByName(_cfg.cf_host, resolved)) {
            _log.log("Falha DNS hostByName");
            _wifi.notifyFailure();
            return false;
        }
        String s = resolved.toString();
        if (s.length() >= len) {
            _log.log("IP DNS invalido");
            return false;
        }
        strlcpy(out, s.c_str(), len);
        return true;
    }

    void _patchRecords(const char* ip) {
        for (uint8_t i = 0; i < _cfg.cf_record_count; i++) {
            String url = "https://api.cloudflare.com/client/v4/zones/" +
                         String(_cfg.cf_zone) + "/dns_records/" +
                         String(_cfg.cf_records[i]);

            WiFiClientSecure client;
            client.setInsecure();
            HTTPClient http;
            http.setTimeout(5000);
            http.begin(client, url);
            http.addHeader("Authorization", "Bearer " + String(_cfg.cf_token));
            http.addHeader("Content-Type", "application/json");

            String payload = "{\"content\":\"";
            payload += ip;
            payload += "\"}";

            int code = http.PATCH(payload);
            if (code > 0) {
                String resp = http.getString();
                StaticJsonDocument<768> doc;
                if (!deserializeJson(doc, resp)) {
                    bool ok = doc["success"] | false;
                    if (ok) {
                        _log.log("DNS atualizado!");
                        _cfg.lastDnsUpdate = millis();
                        _wifi.notifySuccess();
                    } else {
                        const char* msg = doc["errors"][0]["message"] | "Erro desconhecido";
                        int ec = doc["errors"][0]["code"] | 0;
                        _log.log("DNS erro %d: %s", ec, msg);
                    }
                } else {
                    _log.log("DNS resposta JSON invalida");
                }
            } else {
                _log.log("DNS update error: %d", code);
            }
            http.end();
        }
    }
};
