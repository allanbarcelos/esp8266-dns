#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Config.h"
#include "Logger.h"
#include "PublicIPResolver.h"
#include "IoTService.h"

// Notifica o IP público via ntfy.sh no boot e sempre que ele mudar.
// Descobre o IP sozinho: não depende do Cloudflare estar configurado.
// O mesmo tópico leva o sensor, com o prefixo [sensor] ou [ip].
class NtfyNotifier {
public:
    NtfyNotifier(Config& cfg, Logger& log, PublicIPResolver& resolver,
                 IoTService* iot = nullptr)
        : _cfg(cfg), _log(log), _resolver(resolver), _iot(iot),
          _lastAttempt(0), _attempted(false) {
        _lastSent[0] = '\0';
    }

    void tick(unsigned long now) {
        if (_cfg.ntfy_topic[0] == '\0') return;
        if (_attempted && now - _lastAttempt < RETRY_INTERVAL) return;

        _attempted   = true;
        _lastAttempt = now;

        if (_iot && _iot->pending()) {
            if (_post(_iot->message())) _iot->ack();
            return;
        }

        char ip[16];
        if (!_resolver.resolve(ip, sizeof(ip), now)) return;

        strlcpy(_cfg.publicIP, ip, sizeof(_cfg.publicIP));
        if (strcmp(ip, _lastSent) == 0) return;

        char body[24];
        snprintf(body, sizeof(body), "[ip] %s", ip);
        if (_post(body))
            strlcpy(_lastSent, ip, sizeof(_lastSent));
    }

    // Tópicos ntfy: 1–64 caracteres [A-Za-z0-9_-]
    static bool isValidTopic(const String& topic) {
        if (topic.length() == 0 || topic.length() >= sizeof(Config::ntfy_topic)) return false;
        for (size_t i = 0; i < topic.length(); i++) {
            char c = topic.c_str()[i];
            if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
        }
        return true;
    }

    static constexpr unsigned long RETRY_INTERVAL = 60000UL; // entre tentativas

private:
    Config&           _cfg;
    Logger&           _log;
    PublicIPResolver& _resolver;
    IoTService*       _iot;

    char          _lastSent[16];
    unsigned long _lastAttempt;
    bool          _attempted;

    bool _post(const char* body) {
        if (!PublicIPResolver::memoryOk()) {
            _log.log("ntfy: memoria insuficiente");
            return false;
        }
        TlsClient client;
        client.setInsecure(); // o conteúdo (IP público) não é sigiloso
        if (!client.prepare(5000)) {
            _log.log("ntfy: TLS indisponivel");
            return false;
        }
        HTTPClient http;
        http.setTimeout(5000);

        String url = "https://ntfy.sh/";
        url += _cfg.ntfy_topic;

        if (!http.begin(client, url)) {
            _log.log("ntfy: falha ao conectar");
            return false;
        }

        http.addHeader("Content-Type", "text/plain");
        int code = http.POST(body);
        http.end();

        if (code >= 200 && code < 300) {
            _log.log("ntfy enviado (%d)", code);
            return true;
        }
        _log.log("ntfy erro: %d", code);
        return false;
    }
};
