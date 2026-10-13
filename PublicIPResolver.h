// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Config.h"
#include "Logger.h"
#include "Clock.h"
#include "CloudflareCA.h"
#include "ReverseDNS.h"

// O connect do WiFiClientSecure usa o timeout do contexto BearSSL (_owned).
// HTTPClient::setTimeout grava só no wrapper, e o contexto fica nos 15 s
// do core: cada host inacessível trava o loop por esse tempo.
class TlsClient : public WiFiClientSecure {
public:
    // O buffer padrão do BearSSL é 16 KB. Neste aparelho sobram ~30 KB de
    // heap: a alocação fragmenta a memória, o watchdog reinicia o ESP e,
    // sem bloco contíguo, o handshake gera Exception.
    static constexpr uint16_t RX_BUF = 4096;
    static constexpr uint16_t TX_BUF = 512;

    // Só ECDHE com AES-GCM, que todos os servidores usados aceitam
    // (Cloudflare, GitHub, ntfy.sh, ipify, icanhazip, ifconfig.co). As
    // demais cifras nem estão no firmware (TlsTrim.h): anunciar uma delas
    // deixaria o servidor escolher o que o aparelho não sabe fazer.
    static constexpr int SUITE_COUNT = 4;

    bool prepare(unsigned long ms, uint16_t rx = RX_BUF, uint16_t tx = TX_BUF) {
        if (!_owned) return false;
        setTimeout(ms);
        _owned->setTimeout(ms);
        setBufferSizes(rx, tx);
        return setCiphers(_SUITES, SUITE_COUNT);
    }

private:
    // ECDHE_ECDSA e ECDHE_RSA, AES_128_GCM_SHA256 e AES_256_GCM_SHA384
    static constexpr uint16_t _SUITES[SUITE_COUNT] = {0xC02B, 0xC02F, 0xC02C, 0xC030};
};

// IP público por HTTPS. Os três provedores devolvem só o IPv4 e usam a
// cadeia WE1 → GTS Root R4 — a mesma já validada em campo por
// api.cloudflare.com, com as raízes de CloudflareCA.h. Serviço fora
// dessa cadeia não entra: o TLS falharia neste aparelho.
//
// Consulta a cada CHECK_INTERVAL, com ou sem DNS e ntfy configurados; a
// primeira resposta IPv4 válida encerra a busca (uma sessão TLS por
// ciclo). O resultado fica no Config: DNS, ntfy e painel leem dali. O log
// só recebe mudanças: IP novo, ou passar a falhar.
static const char _IP_SVC_0[] PROGMEM = "https://api4.ipify.org";
static const char _IP_SVC_1[] PROGMEM = "https://icanhazip.com";
static const char _IP_SVC_2[] PROGMEM = "https://ifconfig.co/ip";

static const char* const IP_SERVICES[] PROGMEM = {
    _IP_SVC_0, _IP_SVC_1, _IP_SVC_2
};
static const uint8_t NUM_IP_SERVICES = 3;

class PublicIPResolver {
public:
    PublicIPResolver(Config& cfg, Logger& log, Clock& clock)
        : _cfg(cfg), _log(log), _clock(clock) {}

    void tick(unsigned long now) {
        _rdns.tick(now);
        if (!due(now)) return;
        _checked   = true;
        _lastCheck = now;
        _check(now);
    }

    // A próxima chamada de tick vai usar a rede (TLS)
    bool due(unsigned long now) const {
        return WiFi.isConnected() && _clock.synced() &&
               (!_checked || now - _lastCheck >= (_failing ? RETRY_INTERVAL : CHECK_INTERVAL));
    }

    static constexpr unsigned long CHECK_INTERVAL  = 300000UL;  // 5 min
    static constexpr unsigned long RETRY_INTERVAL  = 60000UL;   // depois de falhar
    static constexpr unsigned long CONNECT_TIMEOUT = 4000UL;
    static constexpr uint32_t      MIN_TLS_HEAP    = 18000;
    static constexpr uint32_t      MIN_TLS_BLOCK   = 8192;

    // Heap baixo ou sem bloco contíguo: o BearSSL não consegue o handshake
    // e o core gera Exception em vez de devolver erro.
    static bool memoryOk() {
        return ESP.getFreeHeap() >= MIN_TLS_HEAP &&
               ESP.getMaxFreeBlockSize() >= MIN_TLS_BLOCK;
    }

private:
    Config&       _cfg;
    Logger&       _log;
    Clock&        _clock;
    ReverseDNS    _rdns;
    unsigned long _lastCheck = 0;
    bool          _checked   = false;
    bool          _failing   = false;

    void _check(unsigned long now) {
        char ip[16];
        if (!memoryOk()) {
            _fail(PSTR("IP publico: memoria insuficiente"));
            return;
        }
        BearSSL::X509List trust;
        cloudflareRoots(trust);
        for (uint8_t i = 0;; i++) {
            if (i == NUM_IP_SERVICES) {
                _fail(PSTR("IP publico: nenhum servico respondeu"));
                return;
            }
            char url[48];
            strcpy_P(url, (char*)pgm_read_ptr(&IP_SERVICES[i]));
            if (_query(trust, url, ip)) break;
            yield();
        }

        _failing = false;
        _cfg.ipChecked = now;
        if (strcmp(ip, _cfg.publicIP) == 0) return;
        if (_cfg.publicIP[0])
            _log.log(PSTR("IP publico mudou: %s -> %s"), _cfg.publicIP, ip);
        else
            _log.log(PSTR("IP publico: %s"), ip);
        strlcpy(_cfg.publicIP, ip, sizeof(_cfg.publicIP));
        _cfg.ipChanged = now;
        _rdns.lookup(ip, _cfg.publicHost, sizeof(_cfg.publicHost), now);
    }

    // Só a primeira falha depois de um acerto vai ao log
    void _fail(PGM_P msg) {
        if (!_failing) _log.log(msg);
        _failing = true;
    }

    bool _query(const BearSSL::X509List& trust, const char* url, char* out) {
        TlsClient client;
        client.setTrustAnchors(&trust);
        client.setX509Time(_clock.now());
        if (!client.prepare(CONNECT_TIMEOUT)) return false;

        HTTPClient http;
        http.setTimeout(CONNECT_TIMEOUT);
        if (!http.begin(client, url)) return false;
        http.addHeader("User-Agent", "ESP8266");

        int code = http.GET();
        String body = (code == HTTP_CODE_OK) ? http.getString() : String();
        http.end();

        body.trim();
        IPAddress ip;
        if (code != HTTP_CODE_OK || !ip.fromString(body)) return false;
        strlcpy(out, ip.toString().c_str(), 16);
        return true;
    }
};
