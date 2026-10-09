#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Logger.h"

// Tabela PROGMEM — adicione serviços aqui sem alterar a lógica de resolve()
static const char _IP_SVC_0[] PROGMEM = "http://api.ipify.org";
static const char _IP_SVC_1[] PROGMEM = "http://ifconfig.me/ip";
static const char _IP_SVC_2[] PROGMEM = "http://checkip.amazonaws.com";
static const char _IP_SVC_3[] PROGMEM = "http://v4.ident.me";

static const char* const IP_SERVICES[] PROGMEM = {
    _IP_SVC_0, _IP_SVC_1, _IP_SVC_2, _IP_SVC_3
};
static const uint8_t NUM_IP_SERVICES = 4;

class PublicIPResolver {
public:
    explicit PublicIPResolver(Logger& log) : _log(log) {}

    // Tenta cada serviço em ordem. Só escreve em out[len] quando a resposta
    // é um IPv4 válido — em falha, out permanece intacto.
    bool resolve(char* out, size_t len) {
        for (uint8_t i = 0; i < NUM_IP_SERVICES; i++) {
            char url[40];
            strcpy_P(url, (char*)pgm_read_ptr(&IP_SERVICES[i]));

            if (_query(url, out, len)) return true;
            yield();
        }

        _log.log("Todos os servicos de IP falharam");
        return false;
    }

private:
    Logger& _log;

    bool _query(const char* url, char* out, size_t len) {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(4000);

        if (!http.begin(client, url)) {
            _log.log("IP: falha ao conectar %s", url);
            return false;
        }

        int code = http.GET();
        String body = (code == HTTP_CODE_OK) ? http.getString() : String();
        http.end();

        if (code != HTTP_CODE_OK) {
            _log.log("IP: %s HTTP %d", url, code);
            return false;
        }

        body.trim();
        IPAddress ip;
        if (!ip.fromString(body)) {
            _log.log("IP: resposta invalida de %s", url);
            return false;
        }

        String normalized = ip.toString();
        if (normalized.length() >= len) return false;
        strlcpy(out, normalized.c_str(), len);
        return true;
    }
};
