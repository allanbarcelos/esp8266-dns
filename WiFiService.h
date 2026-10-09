#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "Config.h"
#include "Logger.h"

class WiFiService {
public:
    WiFiService(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {}

    void begin() {
        if (_cfg.ssid.length() == 0) {
            _startConfigMode(WIFI_AP);
            return;
        }

        WiFi.mode(WIFI_STA);
        WiFi.persistent(false); // credenciais ficam no LittleFS; evita desgaste da flash
        WiFi.setAutoReconnect(true);
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
            // AP + STA: o painel fica acessível e a estação segue tentando
            // conectar em segundo plano (autoReconnect).
            _log.log("Falha na conexao");
            _startConfigMode(WIFI_AP_STA);
        }
    }

    void registerEvents() {
        // onEvent() no core 3.1.2 só aceita ponteiro de função (sem captura).
        // Os handlers específicos por evento aceitam std::function e capturam this.
        _evtGotIP = WiFi.onStationModeGotIP([this](const WiFiEventStationModeGotIP&) {
            _log.log("Wi-Fi conectado: %s", WiFi.localIP().toString().c_str());
            notifySuccess();
            if (_configMode) {
                WiFi.mode(WIFI_STA);
                _configMode = false;
                _log.log("Modo AP desligado");
            }
        });

        _evtDisconnected = WiFi.onStationModeDisconnected(
            [this](const WiFiEventStationModeDisconnected&) {
                _log.log("Wi-Fi desconectado");
            }
        );
    }

    // Chamado a cada loop() — verifica saúde e processa reconexão.
    // Em modo de configuração nada é feito: forçar WIFI_STA derrubaria o AP.
    void tick(unsigned long now) {
        if (_configMode) return;

        if (now - _lastStatusCheck >= STATUS_INTERVAL) {
            _lastStatusCheck = now;
            if (WiFi.status() != WL_CONNECTED) {
                _log.log("Wi-Fi desconectado");
                _startReconnect(now);
            }
        }

        if (isConnected() && now - _lastInternetCheck >= INTERNET_INTERVAL) {
            _lastInternetCheck = now;
            if (_hasInternet()) {
                notifySuccess();
            } else {
                _log.log("Wi-Fi conectado, sem internet");
                notifyFailure();
            }
        }

        _processReconnect(now);
    }

    // Verifica restart agendado e watchdogs de memória/uptime
    void tickRestart(unsigned long now) {
        if (!_restartPending && !_otaInProgress) {
            if (now - _lastMemCheck >= MEM_CHECK_INTERVAL) {
                _lastMemCheck = now;
                _checkMemory();
            }
            if (now > DAILY_RESTART) {
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
        if (++_failCount >= MAX_FAILURES) {
            _log.log("Falhas repetidas de Wi-Fi, reiniciando");
            scheduleRestart(1000);
        }
    }

    bool isConnected()    const { return WiFi.status() == WL_CONNECTED; }
    bool restartPending() const { return _restartPending; }
    bool inConfigMode()   const { return _configMode; }

    // Sinaliza que há OTA em progresso (inibe watchdogs de memória e uptime)
    void setOTAInProgress(bool v) { _otaInProgress = v; }

    static constexpr unsigned long STATUS_INTERVAL    = 15000UL;
    static constexpr unsigned long INTERNET_INTERVAL  = 60000UL;
    static constexpr unsigned long MEM_CHECK_INTERVAL = 10000UL;
    static constexpr unsigned long DAILY_RESTART      = 86400000UL;
    static constexpr unsigned long RECONNECT_DELAY    = 500UL;
    static constexpr uint8_t       MAX_FAILURES       = 5;
    static constexpr uint8_t       MEM_STRIKES        = 3;
    static constexpr uint32_t      MIN_FREE_HEAP      = 15000;
    static constexpr uint8_t       MAX_FRAGMENTATION  = 50;

private:
    Config& _cfg;
    Logger& _log;

    uint8_t       _failCount         = 0;
    bool          _configMode        = false;
    bool          _reconnecting      = false;
    unsigned long _reconnectAt       = 0;
    bool          _restartPending    = false;
    unsigned long _restartAt         = 0;
    unsigned long _lastStatusCheck   = 0;
    unsigned long _lastInternetCheck = 0;
    unsigned long _lastMemCheck      = 0;
    uint8_t       _memStrikes        = 0;
    bool          _otaInProgress     = false;

    WiFiEventHandler _evtGotIP;
    WiFiEventHandler _evtDisconnected;

    void _startConfigMode(WiFiMode_t mode) {
        WiFi.mode(mode);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        _configMode = true;
        _log.log("Modo AP iniciado: %s", AP_SSID);
    }

    // Só reinicia se a memória ficar crítica em verificações consecutivas;
    // picos momentâneos (ex.: handshake TLS) não disparam reboot.
    void _checkMemory() {
        bool critical = ESP.getFreeHeap() < MIN_FREE_HEAP ||
                        ESP.getHeapFragmentation() > MAX_FRAGMENTATION;
        if (!critical) {
            _memStrikes = 0;
            return;
        }
        if (++_memStrikes >= MEM_STRIKES) {
            _log.log("Memoria critica detectada");
            scheduleRestart(1000);
        }
    }

    void _startReconnect(unsigned long now) {
        if (_reconnecting) return;
        notifyFailure();
        WiFi.disconnect();
        _reconnecting = true;
        _reconnectAt = now + RECONNECT_DELAY;
        _log.log("Iniciando reconexao Wi-Fi");
    }

    void _processReconnect(unsigned long now) {
        if (!_reconnecting) return;
        if ((long)(now - _reconnectAt) < 0) return;
        _log.log("Tentando reconectar Wi-Fi");
        WiFi.mode(WIFI_STA);
        WiFi.begin(_cfg.ssid.c_str(), _cfg.pass.c_str());
        _reconnecting = false;
    }

    bool _hasInternet() {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(3000);
        if (!http.begin(client, "http://clients3.google.com/generate_204"))
            return false;
        int code = http.GET();
        http.end();
        return code == 204;
    }
};
