// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
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
        _keepRadioAwake();
        WiFi.persistent(false); // credenciais ficam no LittleFS; evita desgaste da flash
        WiFi.setAutoReconnect(true);
        WiFi.begin(_cfg.ssid.c_str(), _cfg.pass.c_str());

        _log.log(PSTR("Conectando ao Wi-Fi: %s"), _cfg.ssid.c_str());
        // Sem esperar aqui: o tick() acompanha a associação e o loop segue.
        _connecting = true;
        _connectSince = millis();
    }

    void registerEvents() {
        // onEvent() no core 3.1.2 só aceita ponteiro de função (sem captura).
        // Os handlers específicos por evento aceitam std::function e capturam this.
        _evtGotIP = WiFi.onStationModeGotIP([this](const WiFiEventStationModeGotIP&) {
            _log.log(PSTR("Wi-Fi conectado: %s"), WiFi.localIP().toString().c_str());
            notifySuccess();
            if (_configMode) {
                WiFi.mode(WIFI_STA);
                _keepRadioAwake();
                _configMode = false;
                _log.log(PSTR("Modo AP desligado"));
            }
        });

        _evtDisconnected = WiFi.onStationModeDisconnected(
            [this](const WiFiEventStationModeDisconnected&) {
                _log.log(PSTR("Wi-Fi desconectado"));
            }
        );
    }

    // Chamado a cada loop() — verifica a associação e processa reconexão.
    // Em modo de configuração nada é feito: forçar WIFI_STA derrubaria o AP.
    //
    // Não há sonda de internet. Perda de associação reconecta e, após 5
    // falhas, reinicia. WAN fora do ar fica com quem precisa dela (busca de
    // IP, DNS, ntfy), que tenta de novo no ciclo seguinte. Reiniciar porque
    // um host externo não responde derrubaria aparelhos com a rede saudável.
    void tick(unsigned long now) {
        if (_connecting) {
            _tickConnect(now);
            return;
        }
        if (_configMode) return;

        if (now - _lastStatusCheck >= STATUS_INTERVAL) {
            _lastStatusCheck = now;
            if (WiFi.status() != WL_CONNECTED) {
                _log.log(PSTR("Wi-Fi desconectado"));
                _startReconnect(now);
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
                _log.log(PSTR("Reinicio diario agendado"));
                scheduleRestart(1000);
            }
        }

        if (_restartPending && (long)(now - _restartAt) >= 0) {
            ESP.restart();
        }
    }

    void scheduleRestart(unsigned long delayMs = 1000) {
        if (_restartPending) return;
        _log.log(PSTR("Reinicio agendado em %lu ms"), delayMs);
        _restartPending = true;
        _restartAt = millis() + delayMs;
    }

    void notifySuccess() { _failCount = 0; }

    void notifyFailure() {
        if (++_failCount >= MAX_FAILURES) {
            _log.log(PSTR("Falhas repetidas de Wi-Fi, reiniciando"));
            scheduleRestart(1000);
        }
    }

    bool isConnected()    const { return WiFi.status() == WL_CONNECTED; }
    bool restartPending() const { return _restartPending; }
    bool inConfigMode()   const { return _configMode; }

    // Sinaliza que há OTA em progresso (inibe watchdogs de memória e uptime)
    void setOTAInProgress(bool v) { _otaInProgress = v; }

    static constexpr unsigned long CONNECT_TIMEOUT    = 10000UL;
    static constexpr unsigned long STATUS_INTERVAL    = 15000UL;
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
    bool          _connecting        = false;
    unsigned long _connectSince      = 0;
    bool          _reconnecting      = false;
    unsigned long _reconnectAt       = 0;
    bool          _restartPending    = false;
    unsigned long _restartAt         = 0;
    unsigned long _lastStatusCheck   = 0;
    unsigned long _lastMemCheck      = 0;
    uint8_t       _memStrikes        = 0;
    bool          _otaInProgress     = false;

    WiFiEventHandler _evtGotIP{};
    WiFiEventHandler _evtDisconnected{};

    // O padrão do STA é modem sleep. Em vários roteadores (inclusive CPE 4G)
    // o rádio dorme entre os beacons e a resposta do handshake se perde:
    // o connect estoura o timeout mesmo com a associação de pé.
    void _keepRadioAwake() { WiFi.setSleepMode(WIFI_NONE_SLEEP); }

    // Primeira associação após o boot: espera até CONNECT_TIMEOUT sem
    // bloquear. Sem conexão, AP + STA: o painel fica acessível e a estação
    // segue tentando em segundo plano (autoReconnect).
    void _tickConnect(unsigned long now) {
        if (WiFi.status() == WL_CONNECTED) {
            _connecting = false;
            _log.log(PSTR("Conectado! IP: %s"), WiFi.localIP().toString().c_str());
            return;
        }
        if (now - _connectSince < CONNECT_TIMEOUT) return;
        _connecting = false;
        _log.log(PSTR("Falha na conexao"));
        _startConfigMode(WIFI_AP_STA);
    }

    void _startConfigMode(WiFiMode_t mode) {
        WiFi.mode(mode);
        _keepRadioAwake();
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        _configMode = true;
        _log.log(PSTR("Modo AP iniciado: %s"), AP_SSID);
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
            _log.log(PSTR("Memoria critica detectada (heap %u, frag %u%%)"),
                     ESP.getFreeHeap(), ESP.getHeapFragmentation());
            scheduleRestart(1000);
        }
    }

    void _startReconnect(unsigned long now) {
        if (_reconnecting) return;
        notifyFailure();
        WiFi.disconnect();
        _reconnecting = true;
        _reconnectAt = now + RECONNECT_DELAY;
        _log.log(PSTR("Iniciando reconexao Wi-Fi"));
    }

    void _processReconnect(unsigned long now) {
        if (!_reconnecting) return;
        if ((long)(now - _reconnectAt) < 0) return;
        _log.log(PSTR("Tentando reconectar Wi-Fi"));
        WiFi.mode(WIFI_STA);
        _keepRadioAwake();
        WiFi.begin(_cfg.ssid.c_str(), _cfg.pass.c_str());
        _reconnecting = false;
    }
};
