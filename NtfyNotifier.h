#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Config.h"
#include "Logger.h"

// Notifica o IP público via ntfy.sh no boot e sempre que ele mudar.
class NtfyNotifier {
public:
    NtfyNotifier(Config& cfg, Logger& log)
        : _cfg(cfg), _log(log), _lastAttempt(0), _attempted(false) {
        _lastSent[0] = '\0';
    }

    void tick(unsigned long now) {
        if (_cfg.publicIP[0] == '\0' || _cfg.ntfy_topic[0] == '\0') return;
        if (strcmp(_cfg.publicIP, _lastSent) == 0) return;
        if (_attempted && now - _lastAttempt < RETRY_INTERVAL) return;

        _attempted   = true;
        _lastAttempt = now;
        if (_send(_cfg.publicIP)) {
            strlcpy(_lastSent, _cfg.publicIP, sizeof(_lastSent));
        }
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

    static constexpr unsigned long RETRY_INTERVAL = 60000UL; // 1 min após falha

private:
    Config& _cfg;
    Logger& _log;

    char          _lastSent[16];
    unsigned long _lastAttempt;
    bool          _attempted;

    bool _send(const char* ip) {
        WiFiClientSecure client;
        client.setInsecure(); // o conteúdo (IP público) não é sigiloso
        HTTPClient http;
        http.setTimeout(5000);

        String url = "https://ntfy.sh/";
        url += _cfg.ntfy_topic;

        if (!http.begin(client, url)) {
            _log.log("ntfy: falha ao conectar");
            return false;
        }

        http.addHeader("Content-Type", "text/plain");
        String body = "IP publico atual: ";
        body += ip;

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
