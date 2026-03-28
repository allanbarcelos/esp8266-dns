#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Logger.h"

// Tabela PROGMEM — adicione serviços aqui sem alterar a lógica de resolve() (OCP)
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

    // Tenta cada serviço em ordem; escreve o IP em out[len].
    // Retorna true em sucesso. Não toca em Config — quem chama decide o que fazer.
    bool resolve(char* out, size_t len) {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(4000);

        for (uint8_t i = 0; i < NUM_IP_SERVICES; i++) {
            char url[40];
            strcpy_P(url, (char*)pgm_read_ptr(&IP_SERVICES[i]));

            _log.log("Tentando IP via: %s", url);

            if (!http.begin(client, url)) continue;

            int code = http.GET();
            if (code == HTTP_CODE_OK) {
                WiFiClient* stream = http.getStreamPtr();
                size_t sz = 0;
                while (stream->available() && sz < len - 1) {
                    char c = stream->read();
                    if (isDigit(c) || c == '.') out[sz++] = c;
                }
                out[sz] = '\0';
                http.end();
                if (sz > 6) return true;
            } else {
                http.end();
            }

            yield();
        }

        _log.log("Todos os servicos de IP falharam");
        return false;
    }

private:
    Logger& _log;
};
