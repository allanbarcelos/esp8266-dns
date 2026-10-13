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
#include "PublicIPResolver.h"  // TlsClient

// Mantém o registro A do Cloudflare apontando para o IP público.
//
// O IP vem do PublicIPResolver (Config::publicIP). O já sincronizado fica
// em memória: não dependemos do resolver DNS local (que tem cache/TTL e
// devolve IPs da Cloudflare em registros proxied). Após um boot, o
// primeiro IP conhecido sempre gera um PATCH (idempotente); falha tenta de
// novo a cada RETRY_INTERVAL.
class CloudflareDNS {
public:
    CloudflareDNS(Config& cfg, Logger& log, WiFiService& wifi, Clock& clock)
        : _cfg(cfg), _log(log), _wifi(wifi), _clock(clock) {
        _syncedIP[0] = '\0';
    }

    void tick(unsigned long now) {
        if (!due(now)) return;
        _attempted   = true;
        _lastAttempt = now;
        _sync();
    }

    // A próxima chamada de tick vai usar a rede (TLS)
    bool due(unsigned long now) const {
        return configured() && _cfg.publicIP[0] && strcmp(_cfg.publicIP, _syncedIP) != 0 &&
               (!_attempted || now - _lastAttempt >= RETRY_INTERVAL) &&
               _wifi.isConnected() && _clock.synced();  // TLS precisa de relógio válido
    }

    bool configured() const {
        return _cfg.cf_token[0] && _cfg.cf_zone[0] && _cfg.cf_record[0];
    }

    const char* syncedIP() const { return _syncedIP; }

    static constexpr unsigned long RETRY_INTERVAL = 60000UL;

private:
    Config&       _cfg;
    Logger&       _log;
    WiFiService&  _wifi;
    Clock&        _clock;
    unsigned long _lastAttempt = 0;
    bool          _attempted   = false;
    char          _syncedIP[16];

    void _sync() {
        char ip[16];
        strlcpy(ip, _cfg.publicIP, sizeof(ip));
        _log.log(PSTR("Atualizando DNS para %s"), ip);
        BearSSL::X509List trust;
        cloudflareRoots(trust);
        if (_patchRecord(trust, _cfg.cf_record, ip)) {
            strlcpy(_syncedIP, ip, sizeof(_syncedIP));
            _cfg.lastDnsUpdate = millis();
            _wifi.notifySuccess();
            _log.log(PSTR("DNS atualizado!"));
        }
    }

    bool _patchRecord(const BearSSL::X509List& trust, const char* record, const char* ip) {
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
        client.stop();  // o keep-alive do HTTPClient manteria a sessão TLS aberta

        if (code <= 0) {
            _log.log(PSTR("DNS erro de conexao: %d"), code);
            return false;
        }

        // Lida no lugar (char*), como as mensagens do ntfy: um só leitor de
        // JSON no firmware, e as strings ficam na resposta. O documento
        // guarda só a estrutura, uma posição por valor, e a TLS já foi
        // fechada: a heap está livre.
        DynamicJsonDocument doc(JSON_ARRAY_SIZE(_values(resp)));
        DeserializationError err = deserializeJson(doc, resp.begin());
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

    // Teto de valores no JSON: cada valor depois do primeiro de um objeto
    // ou lista vem após uma vírgula, e o primeiro, após { ou [. Vírgula
    // dentro de texto só aumenta a conta.
    static size_t _values(const String& json) {
        size_t n = 0;
        for (const char* p = json.c_str(); *p; p++)
            n += *p == ',' || *p == '{' || *p == '[';
        return n;
    }
};
