#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <ArduinoJson.h>
#include "Logger.h"
#include "WiFiService.h"

class OTAService {
public:
    OTAService(Logger& log, WiFiService& wifi)
        : _log(log), _wifi(wifi),
          _inProgress(false), _contentLength(0), _written(0),
          _lastCheck(0) {}

    void tick(unsigned long now) {
        if (_inProgress) {
            _streamChunk();
            return;
        }
        if (_wifi.restartPending()) return;
        if (now >= DEADLINE) return;
        if (now - _lastCheck < CHECK_INTERVAL) return;

        _lastCheck = now;

        String latest;
        if (!_checkVersion(latest)) return;
        if (latest == firmware_version) return;

        _log.log("Nova versao encontrada: %s", latest.c_str());
        _startDownload(latest);
    }

    bool   inProgress()    const { return _inProgress; }
    size_t bytesWritten()  const { return _written; }
    int    contentLength() const { return _contentLength; }

    static const unsigned long CHECK_INTERVAL = 3600000UL;  // 1 hora
    static const unsigned long DEADLINE       = 82800000UL; // ~23 h

private:
    Logger&      _log;
    WiFiService& _wifi;

    bool             _inProgress;
    HTTPClient       _http;
    WiFiClientSecure _client;
    int              _contentLength;
    size_t           _written;
    unsigned long    _lastCheck;

    // (constantes públicas declaradas acima)

    // Consulta a GitHub Releases API para obter o tag_name do último release
    bool _checkVersion(String& latest) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(5000);

        if (!http.begin(client, "https://api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest")) {
            _log.log("Falha ao verificar versao");
            return false;
        }
        http.addHeader("Accept", "application/vnd.github+json");
        http.addHeader("User-Agent", "ESP8266");

        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            _log.log("Erro HTTP versao: %d", code);
            http.end();
            return false;
        }

        // Extrai apenas tag_name sem alocar um JsonDocument grande
        StaticJsonDocument<128> filter;
        filter["tag_name"] = true;

        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        http.end();

        if (err) {
            _log.log("Erro JSON versao: %s", err.c_str());
            return false;
        }

        latest = doc["tag_name"].as<String>();
        latest.trim();
        return latest.length() > 0;
    }

    bool _startDownload(const String& version) {
        String url = "https://github.com/allanbarcelos/esp8266-dns/releases/download/";
        url += version;
        url += "/firmware.bin";

        _log.log("URL firmware: %s", url.c_str());

        _client.setInsecure();
        _http.setTimeout(5000);

        if (!_http.begin(_client, url)) {
            _log.log("Falha ao iniciar OTA");
            return false;
        }

        int code = _http.GET();
        if (code != HTTP_CODE_OK) {
            _log.log("HTTP OTA erro: %d", code);
            _http.end();
            return false;
        }

        _contentLength = _http.getSize();
        if (_contentLength <= 0) {
            _log.log("Tamanho firmware invalido");
            _http.end();
            return false;
        }

        if (!Update.begin(_contentLength)) {
            _log.log("Sem espaco para OTA");
            _http.end();
            return false;
        }

        _written = 0;
        _inProgress = true;
        _wifi.setOTAInProgress(true);
        _log.log("OTA iniciado: %d bytes", _contentLength);
        return true;
    }

    void _streamChunk() {
        if (!_wifi.isConnected()) {
            _log.log("Wi-Fi caiu durante OTA, abortando");
            Update.end(false);
            _http.end();
            _inProgress = false;
            _wifi.setOTAInProgress(false);
            _wifi.notifyFailure();
            return;
        }

        WiFiClient* stream = _http.getStreamPtr();
        if (!stream->available()) return;

        uint8_t buf[512];
        int len = stream->readBytes(buf, sizeof(buf));
        if (len > 0) {
            _written += Update.write(buf, len);
            _log.log("OTA: %zu / %d bytes", _written, _contentLength);
        }

        if (_written >= (size_t)_contentLength) {
            if (!Update.end()) {
                _log.log("Erro OTA: %s", Update.getErrorString().c_str());
            } else {
                _log.log("OTA concluido! Reiniciando...");
                _wifi.scheduleRestart(1000);
            }
            _http.end();
            _inProgress = false;
            _wifi.setOTAInProgress(false);
        }
    }
};
