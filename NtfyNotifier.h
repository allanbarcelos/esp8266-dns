#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Config.h"
#include "Logger.h"

class NtfyNotifier {
public:
    NtfyNotifier(Config& cfg, Logger& log)
        : _cfg(cfg), _log(log), _lastSend(0), _sentAfterBoot(false) {}

    void tick(unsigned long now) {
        if (strlen(_cfg.publicIP) == 0) return;
        if (strlen(_cfg.ntfy_topic) == 0) return;

        if (!_sentAfterBoot) {
            _send(_cfg.publicIP);
            _sentAfterBoot = true;
            _lastSend = now;
            return;
        }

        if (now - _lastSend >= SEND_INTERVAL) {
            _lastSend = now;
            _send(_cfg.publicIP);
        }
    }

    static const unsigned long SEND_INTERVAL = 3600000UL; // 1 hora

private:
    Config& _cfg;
    Logger& _log;

    unsigned long _lastSend;
    bool          _sentAfterBoot;

    void _send(const char* ip) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(5000);

        String url = "https://ntfy.sh/";
        url += _cfg.ntfy_topic;

        if (!http.begin(client, url)) {
            _log.log("ntfy: falha ao conectar");
            return;
        }

        http.addHeader("Content-Type", "text/plain");
        String body = "IP publico atual: ";
        body += ip;

        int code = http.POST(body);
        if (code > 0) {
            _log.log("ntfy enviado (%d)", code);
        } else {
            _log.log("ntfy erro: %d", code);
        }
        http.end();
    }
};
