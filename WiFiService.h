// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <LittleFS.h>
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
        WiFi.persistent(false); // credentials live in LittleFS; avoids flash wear
        WiFi.setAutoReconnect(true);
        if (_cfg.hostname[0]) WiFi.hostname(_cfg.hostname);   // before DHCP
        WiFi.begin(_cfg.ssid.c_str(), _cfg.pass.c_str());

        _log.log(PSTR("Connecting to Wi-Fi: %s"), _cfg.ssid.c_str());
        // No waiting here: tick() follows the association and the loop keeps going.
        _connecting = true;
        _connectSince = millis();
    }

    void registerEvents() {
        // onEvent() in core 3.1.2 only takes a function pointer (no capture).
        // The per-event handlers take std::function and capture this.
        _evtGotIP = WiFi.onStationModeGotIP([this](const WiFiEventStationModeGotIP&) {
            _log.log(PSTR("Wi-Fi connected: %s"), WiFi.localIP().toString().c_str());
            notifySuccess();
            if (_configMode) {
                WiFi.mode(WIFI_STA);
                _keepRadioAwake();
                _configMode = false;
                _log.log(PSTR("AP mode off"));
            }
        });

        _evtDisconnected = WiFi.onStationModeDisconnected(
            [this](const WiFiEventStationModeDisconnected&) {
                _log.log(PSTR("Wi-Fi disconnected"));
            }
        );
    }

    // Called on every loop(): checks the association and handles reconnection.
    // Nothing happens in setup mode: forcing WIFI_STA would drop the AP.
    //
    // There is no internet probe. A lost association reconnects and, after 5
    // failures, restarts. A WAN outage is handled by whoever needs the WAN
    // (IP check, DNS, ntfy), which tries again on the next cycle. Restarting
    // because an outside host does not answer would take down devices on a
    // healthy network.
    void tick(unsigned long now) {
        if (_connecting) {
            _tickConnect(now);
            return;
        }
        if (_configMode) {
            if (now - _lastAnnounce >= AP_ANNOUNCE_MS) _announceAp(now);
            return;
        }

        if (now - _lastStatusCheck >= STATUS_INTERVAL) {
            _lastStatusCheck = now;
            if (WiFi.status() != WL_CONNECTED) {
                _log.log(PSTR("Wi-Fi disconnected"));
                _startReconnect(now);
            }
        }

        _processReconnect(now);
    }

    // Checks a scheduled restart and the memory/uptime watchdogs
    void tickRestart(unsigned long now) {
        if (!_restartPending && !_otaInProgress) {
            if (now - _lastMemCheck >= MEM_CHECK_INTERVAL) {
                _lastMemCheck = now;
                _checkMemory();
            }
            if (now > DAILY_RESTART) {
                _log.log(PSTR("Daily restart scheduled"));
                scheduleRestart(1000);
            }
        }

        if (_restartPending && (long)(now - _restartAt) >= 0) {
            ESP.restart();
        }
    }

    void scheduleRestart(unsigned long delayMs = 1000) {
        if (_restartPending) return;
        _log.log(PSTR("Restart scheduled in %lu ms"), delayMs);
        _restartPending = true;
        _restartAt = millis() + delayMs;
    }

    void notifySuccess() { _failCount = 0; }

    void notifyFailure() {
        if (++_failCount >= MAX_FAILURES) {
            _log.log(PSTR("Repeated Wi-Fi failures, restarting"));
            scheduleRestart(1000);
        }
    }

    bool isConnected()    const { return WiFi.status() == WL_CONNECTED; }
    bool restartPending() const { return _restartPending; }
    bool inConfigMode()   const { return _configMode; }

    // Setup AP password: drawn on the first boot by the hardware RNG and
    // kept in /ap.key. A fixed, public password would let anyone nearby join
    // the AP and set the panel password before the owner. It shows on the
    // serial port and on the panel (Status). -DAP_PASSWORD sets a fixed one.
    const char* apPassword() {
#ifdef AP_PASSWORD
        return AP_PASSWORD;
#else
        if (!_apPass[0]) _loadApKey();
        return _apPass;
#endif
    }

    static constexpr const char*   AP_KEY_PATH    = "/ap.key";
    static constexpr size_t        AP_KEY_LEN     = 12;
    static constexpr unsigned long AP_ANNOUNCE_MS = 30000UL;

    // Flags an OTA in progress (holds off the memory and uptime watchdogs)
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

    char          _apPass[AP_KEY_LEN + 1] = {};
    unsigned long _lastAnnounce      = 0;

    WiFiEventHandler _evtGotIP{};
    WiFiEventHandler _evtDisconnected{};

    // STA defaults to modem sleep. With many routers (4G CPEs included) the
    // radio sleeps between beacons and the handshake reply gets lost: the
    // connect times out even with the association up.
    void _keepRadioAwake() { WiFi.setSleepMode(WIFI_NONE_SLEEP); }

    // First association after boot: waits up to CONNECT_TIMEOUT without
    // blocking. With no connection, AP + STA: the panel stays reachable and
    // the station keeps trying in the background (autoReconnect).
    void _tickConnect(unsigned long now) {
        if (WiFi.status() == WL_CONNECTED) {
            _connecting = false;
            _log.log(PSTR("Connected! IP: %s"), WiFi.localIP().toString().c_str());
            return;
        }
        if (now - _connectSince < CONNECT_TIMEOUT) return;
        _connecting = false;
        _log.log(PSTR("Connection failed"));
        _startConfigMode(WIFI_AP_STA);
    }

    void _startConfigMode(WiFiMode_t mode) {
        WiFi.mode(mode);
        _keepRadioAwake();
        WiFi.softAP(AP_SSID, apPassword());
        _configMode = true;
        _log.log(PSTR("AP mode started: %s"), AP_SSID);
        _announceAp(millis());
    }

    // On the serial port (not the panel log), when the AP comes up and every
    // 30 s: someone who opens the monitor or the web installer later still
    // sees the password
    void _announceAp(unsigned long now) {
        _lastAnnounce = now;
        Serial.printf("AP %s, password: %s\n", AP_SSID, apPassword());
    }

#ifndef AP_PASSWORD
    // Draws 12 characters out of 32 (60 bits), without the ambiguous l, o, 0 and 1
    void _loadApKey() {
        File in = LittleFS.open(AP_KEY_PATH, "r");
        if (in) {
            size_t n = in.readBytes(_apPass, sizeof(_apPass));
            in.close();
            bool ok = n == AP_KEY_LEN;
            for (size_t i = 0; ok && i < AP_KEY_LEN; i++) {
                char c = _apPass[i];
                ok = (c >= 'a' && c <= 'z') || (c >= '2' && c <= '9');
            }
            _apPass[AP_KEY_LEN] = '\0';
            if (ok) return;
        }
        static const char ALPHABET[] PROGMEM = "abcdefghijkmnpqrstuvwxyz23456789";
        for (size_t i = 0; i < AP_KEY_LEN; i++)
            _apPass[i] = (char)pgm_read_byte(&ALPHABET[ESP.random() & 31]);
        _apPass[AP_KEY_LEN] = '\0';
        File out = LittleFS.open(AP_KEY_PATH, "w");
        if (!out) {
            _log.log(PSTR("AP: password not saved"));   // valid until the next boot
            return;
        }
        out.write((const uint8_t*)_apPass, AP_KEY_LEN);
        out.close();
        _log.log(PSTR("AP: new password generated"));
    }
#endif

    // Only restarts if memory stays critical on consecutive checks;
    // momentary spikes (e.g. a TLS handshake) do not trigger a reboot.
    void _checkMemory() {
        bool critical = ESP.getFreeHeap() < MIN_FREE_HEAP ||
                        ESP.getHeapFragmentation() > MAX_FRAGMENTATION;
        if (!critical) {
            _memStrikes = 0;
            return;
        }
        if (++_memStrikes >= MEM_STRIKES) {
            _log.log(PSTR("Critical memory (heap %u, frag %u%%)"),
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
        _log.log(PSTR("Starting Wi-Fi reconnection"));
    }

    void _processReconnect(unsigned long now) {
        if (!_reconnecting) return;
        if ((long)(now - _reconnectAt) < 0) return;
        _log.log(PSTR("Trying to reconnect Wi-Fi"));
        WiFi.mode(WIFI_STA);
        _keepRadioAwake();
        WiFi.begin(_cfg.ssid.c_str(), _cfg.pass.c_str());
        _reconnecting = false;
    }
};
