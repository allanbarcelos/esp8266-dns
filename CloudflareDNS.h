// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareCA.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"

// Mantém os registros A do Cloudflare apontando para o IP público.
//
// O IP já sincronizado é guardado em memória: não dependemos do resolver DNS
// local (que tem cache/TTL e devolve IPs da Cloudflare em registros proxied).
// Após um boot, a primeira verificação sempre envia um PATCH (idempotente).
class CloudflareDNS {
public:
    CloudflareDNS(Config& cfg, Logger& log, WiFiService& wifi,
                  PublicIPResolver& resolver, Clock& clock)
        : _cfg(cfg), _log(log), _wifi(wifi), _resolver(resolver), _clock(clock),
          _lastCheck(0) {
        _syncedIP[0] = '\0';
    }

    void tick(unsigned long now) {
        if (!configured()) return;
        if (now - _lastCheck < CHECK_INTERVAL) return;
        if (!_wifi.isConnected()) return;
        if (!_clock.synced()) return; // TLS precisa de relógio válido
        _lastCheck = now;
        _update(now);
    }

    bool configured() const {
        return _cfg.cf_token[0] && _cfg.cf_zone[0] && _cfg.cf_record_count > 0;
    }

    const char* syncedIP() const { return _syncedIP; }

    static constexpr unsigned long CHECK_INTERVAL = 60000UL;

private:
    Config&           _cfg;
    Logger&           _log;
    WiFiService&      _wifi;
    PublicIPResolver& _resolver;
    Clock&            _clock;
    unsigned long     _lastCheck;
    char              _syncedIP[16];

    void _update(unsigned long now) {
        char ip[16];
        if (!_resolver.resolve(ip, sizeof(ip), now)) {
            _log.log(PSTR("Falha ao obter IP publico"));
            return;
        }
        strlcpy(_cfg.publicIP, ip, sizeof(_cfg.publicIP));

        if (strcmp(ip, _syncedIP) == 0) return;

        _log.log(PSTR("Atualizando DNS para %s"), ip);
        if (_patchRecords(ip)) {
            strlcpy(_syncedIP, ip, sizeof(_syncedIP));
            _cfg.lastDnsUpdate = millis();
            _wifi.notifySuccess();
            _log.log(PSTR("DNS atualizado!"));
        }
    }

    // Retorna true somente se TODOS os registros foram atualizados;
    // caso contrário a próxima verificação tenta de novo.
    bool _patchRecords(const char* ip) {
        BearSSL::X509List trust(CLOUDFLARE_CA);
        bool allOk = true;

        for (uint8_t i = 0; i < _cfg.cf_record_count; i++) {
            if (!_patchRecord(trust, _cfg.cf_records[i], ip)) allOk = false;
            yield();
        }
        return allOk;
    }

    bool _patchRecord(BearSSL::X509List& trust, const char* record, const char* ip) {
        String url = "https://api.cloudflare.com/client/v4/zones/";
        url += _cfg.cf_zone;
        url += "/dns_records/";
        url += record;

        if (!PublicIPResolver::memoryOk()) {
            _log.log(PSTR("DNS: memoria insuficiente"));
            return false;
        }
        TlsClient client;
        client.setTrustAnchors(&trust);
        client.setX509Time(_clock.now());
        if (!client.prepare(5000)) {
            _log.log(PSTR("DNS: TLS indisponivel"));
            return false;
        }

        HTTPClient http;
        http.setTimeout(5000);
        if (!http.begin(client, url)) {
            _log.log(PSTR("DNS: falha ao conectar"));
            return false;
        }
        http.addHeader("Authorization", String("Bearer ") + _cfg.cf_token);
        http.addHeader("Content-Type", "application/json");

        String payload = "{\"content\":\"";
        payload += ip;
        payload += "\"}";

        int code = http.PATCH(payload);
        String resp = (code > 0) ? http.getString() : String();
        http.end();

        if (code <= 0) {
            _log.log(PSTR("DNS erro de conexao: %d"), code);
            return false;
        }

        // Capacidades via macros: corretas tanto no ESP8266 quanto no host (64 bits)
        constexpr size_t SHAPE = JSON_OBJECT_SIZE(2) + JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(2);
        StaticJsonDocument<SHAPE> filter;
        filter["success"] = true;
        filter["errors"][0]["code"]    = true;
        filter["errors"][0]["message"] = true;

        StaticJsonDocument<SHAPE + 160> doc; // + texto da mensagem de erro
        DeserializationError err = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
        if (err || doc["success"].isNull()) {
            _log.log(PSTR("DNS resposta invalida (HTTP %d)"), code);
            return false;
        }

        if (!(doc["success"] | false)) {
            int         ec  = doc["errors"][0]["code"]    | 0;
            const char* msg = doc["errors"][0]["message"] | "Erro desconhecido";
            _log.log(PSTR("DNS erro %d: %s"), ec, msg);
            return false;
        }
        return true;
    }
};
