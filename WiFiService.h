#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Config.h"
#include "Logger.h"

class WiFiService {
public:
    WiFiService(Config& cfg, Logger& log)
        : _cfg(cfg), _log(log),
          _failCount(0),
          _reconnecting(false), _reconnectAt(0),
          _restartPending(false), _restartAt(0),
          _lastHealthCheck(0) {}

    void begin() {
        if (_cfg.ssid.length() == 0) {
            WiFi.mode(WIFI_AP);
            WiFi.softAP("ESP_Config");
            _log.log("Modo AP iniciado: ESP_Config");
            return;
        }

        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.persistent(true);
        WiFi.begin(_cfg.ssid.c_str(), _cfg.pass.c_str());

        _log.log("Conectando ao Wi-Fi: %s", _cfg.ssid.c_str());

        uint8_t attempts = 20;
        while (WiFi.status() != WL_CONNECTED && attempts--) {
            yield();
            delay(500);
        }

        if (WiFi.status() == WL_CONNECTED) {
            _log.log("Conectado! IP: %s", WiFi.localIP().toString().c_str());
        } else {
            _log.log("Falha na conexao. Iniciando modo AP");
            WiFi.mode(WIFI_AP);
            WiFi.softAP("ESP_Config");
        }
    }

    void registerEvents() {
        // onEvent() no core 3.1.2 só aceita ponteiro de função (sem captura).
        // Os handlers específicos por evento aceitam std::function e capturam this.
        _evtGotIP = WiFi.onStationModeGotIP([this](const WiFiEventStationModeGotIP&) {
            _log.log("Wi-Fi conectado: %s", WiFi.localIP().toString().c_str());
            notifySuccess();
            if (WiFi.getMode() != WIFI_STA) {
                WiFi.mode(WIFI_STA);
                _log.log("Modo AP desligado");
            }
        });

        _evtDisconnected = WiFi.onStationModeDisconnected(
            [this](const WiFiEventStationModeDisconnected&) {
                _log.log("Wi-Fi desconectado");
            }
        );
    }

    // Chamado a cada loop() — verifica saúde e processa reconexão
    void tick(unsigned long now) {
        if (now - _lastHealthCheck > 15000UL) {
            _lastHealthCheck = now;

            if (WiFi.status() != WL_CONNECTED) {
                _log.log("Wi-Fi desconectado");
                _startReconnect();
            } else if (!_hasInternet()) {
                _log.log("Wi-Fi conectado, sem internet");
                notifyFailure();
            } else {
                notifySuccess();
            }
        }

        _processReconnect(now);
    }

    // Verifica restart agendado e watchdog de memória/uptime
    void tickRestart(unsigned long now) {
        if (!_restartPending) {
            if (!_otaInProgress) {
                if (ESP.getFreeHeap() < 15000 || ESP.getHeapFragmentation() > 35) {
                    _log.log("Memoria critica detectada");
                    scheduleRestart(1000);
                }
            }
            if (now > 86400000UL) {
                _log.log("Reinicio diario agendado");
                scheduleRestart(1000);
            }
        }

        if (_restartPending && (long)(now - _restartAt) >= 0) {
            ESP.restart();
        }
    }

    void scheduleRestart(unsigned long delayMs = 1000) {
        if (_restartPending) return;
        _log.log("Reinicio agendado em %lu ms", delayMs);
        _restartPending = true;
        _restartAt = millis() + delayMs;
    }

    void notifySuccess() { _failCount = 0; }

    void notifyFailure() {
        if (++_failCount >= 5) {
            _log.log("Falhas repetidas de Wi-Fi, reiniciando");
            scheduleRestart(1000);
        }
    }

    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
    bool restartPending() const { return _restartPending; }

    // Sinaliza ao WiFiService que OTA está em progresso (inibe watchdog de memória)
    void setOTAInProgress(bool v) { _otaInProgress = v; }

private:
    Config& _cfg;
    Logger& _log;

    uint8_t       _failCount;
    bool          _reconnecting;
    unsigned long _reconnectAt;
    bool          _restartPending;
    unsigned long _restartAt;
    unsigned long _lastHealthCheck;
    bool          _otaInProgress = false;

    WiFiEventHandler _evtGotIP;
    WiFiEventHandler _evtDisconnected;

    static const unsigned long RECONNECT_DELAY = 500UL;

    void _startReconnect() {
        if (_reconnecting) return;
        notifyFailure();
        WiFi.disconnect();
        _reconnecting = true;
        _reconnectAt = millis() + RECONNECT_DELAY;
        _log.log("Iniciando reconexao Wi-Fi");
    }

    void _processReconnect(unsigned long now) {
        if (!_reconnecting) return;
        if ((long)(now - _reconnectAt) >= 0) {
            _log.log("Tentando reconectar Wi-Fi");
            WiFi.mode(WIFI_STA);
            WiFi.begin(_cfg.ssid.c_str(), _cfg.pass.c_str());
            _reconnecting = false;
        }
    }

    bool _hasInternet() {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(5000);
        if (!http.begin(client, "http://clients3.google.com/generate_204"))
            return false;
        int code = http.GET();
        http.end();
        return code == 204;
    }
};
