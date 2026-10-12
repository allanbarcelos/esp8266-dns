// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Logger.h"
#include "Clock.h"
#include "CloudflareCA.h"

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

    bool prepare(unsigned long ms, uint16_t rx = RX_BUF, uint16_t tx = TX_BUF) {
        if (!_owned) return false;
        setTimeout(ms);
        _owned->setTimeout(ms);
        setBufferSizes(rx, tx);
        return true;
    }
};

// IP público por HTTPS. Os três provedores devolvem só o IPv4 e usam a
// cadeia WE1 → GTS Root R4 — a mesma já validada em campo por
// api.cloudflare.com, com as raízes de CloudflareCA.h. Serviço fora
// dessa cadeia não entra: o TLS falharia neste aparelho.
//
// A primeira resposta IPv4 válida encerra a busca (uma sessão TLS por
// ciclo). DNS e ntfy compartilham o resultado pelo cache.
static const char _IP_SVC_0[] PROGMEM = "https://api4.ipify.org";
static const char _IP_SVC_1[] PROGMEM = "https://icanhazip.com";
static const char _IP_SVC_2[] PROGMEM = "https://ifconfig.co/ip";

static const char* const IP_SERVICES[] PROGMEM = {
    _IP_SVC_0, _IP_SVC_1, _IP_SVC_2
};
static const uint8_t NUM_IP_SERVICES = 3;

class PublicIPResolver {
public:
    PublicIPResolver(Logger& log, Clock& clock)
        : _log(log), _clock(clock), _cachedAt(0), _failedAt(0), _failed(false) {
        _cached[0] = '\0';
    }

    // Escreve um IPv4 em out somente quando a resposta é válida.
    // Em falha, out permanece intacto. `now` é o mesmo millis() do tick.
    bool resolve(char* out, size_t len, unsigned long now) {
        if (_cached[0] != '\0' && now - _cachedAt < CACHE_TTL)
            return _copyCached(out, len);
        if (_failed && now - _failedAt < CACHE_TTL)
            return false;
        if (!_clock.synced()) {
            _log.log(PSTR("IP: relogio nao sincronizado"));
            return false;
        }

        // Marca a tentativa antes da rede: falha recente não repete o TLS
        // até o próximo ciclo, nem quando DNS e ntfy perguntam no mesmo tick.
        _failed   = true;
        _failedAt = now;

        if (!memoryOk()) {
            _log.log(PSTR("IP: memoria insuficiente"));
            return false;
        }

        BearSSL::X509List trust(CLOUDFLARE_CA);
        for (uint8_t i = 0; i < NUM_IP_SERVICES; i++) {
            char url[48];
            strcpy_P(url, (char*)pgm_read_ptr(&IP_SERVICES[i]));
            if (_query(trust, url, out, len)) {
                strlcpy(_cached, out, sizeof(_cached));
                _cachedAt = now;
                _failed   = false;
                return true;
            }
            yield();
        }

        _log.log(PSTR("Todos os servicos de IP falharam"));
        return false;
    }

    static constexpr unsigned long CACHE_TTL       = 60000UL;
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
    Logger&       _log;
    Clock&        _clock;
    char          _cached[16];
    unsigned long _cachedAt;
    unsigned long _failedAt;
    bool          _failed;

    bool _copyCached(char* out, size_t len) const {
        if (strlen(_cached) >= len) return false;
        strlcpy(out, _cached, len);
        return true;
    }

    bool _query(BearSSL::X509List& trust, const char* url, char* out, size_t len) {
        TlsClient client;
        client.setTrustAnchors(&trust);
        client.setX509Time(_clock.now());
        if (!client.prepare(CONNECT_TIMEOUT)) {
            _log.log(PSTR("IP: TLS indisponivel"));
            return false;
        }

        HTTPClient http;
        http.setTimeout(CONNECT_TIMEOUT);
        if (!http.begin(client, url)) {
            _log.log(PSTR("IP: falha ao conectar %s"), url);
            return false;
        }
        http.addHeader("User-Agent", "ESP8266");

        int code = http.GET();
        String body = (code == HTTP_CODE_OK) ? http.getString() : String();
        http.end();

        if (code != HTTP_CODE_OK) {
            // code <= 0: o TCP/TLS não completou (o core reporta -1).
            if (code <= 0)
                _log.log(PSTR("IP: %s sem conexao (%d)"), url, code);
            else
                _log.log(PSTR("IP: %s HTTP %d"), url, code);
            return false;
        }

        body.trim();
        IPAddress ip;
        if (!ip.fromString(body)) {
            _log.log(PSTR("IP: resposta invalida de %s"), url);
            return false;
        }

        String normalized = ip.toString();
        if (normalized.length() >= len) return false;
        strlcpy(out, normalized.c_str(), len);
        return true;
    }
};
