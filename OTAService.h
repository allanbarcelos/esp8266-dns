#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <ArduinoJson.h>
#include "Logger.h"
#include "Version.h"
#include "WiFiService.h"

// Atualização OTA a partir dos GitHub Releases.
//
// O transporte usa TLS sem validação de certificado, mas o firmware publicado
// pelo CI é assinado (RSA-2048/SHA-256) e o Updater do core verifica a
// assinatura com public.key embutida no build — um binário adulterado é
// rejeitado em Update.end(). Versões mais antigas que a atual são ignoradas,
// impedindo downgrade forçado via MITM na consulta de versão.
//
// A imagem é comprimida com gzip (o eboot descomprime no boot): sem isso,
// atual + nova não cabem juntas nos ~940 KB de área de sketch de 1 MB.
class OTAService {
public:
    OTAService(Logger& log, WiFiService& wifi) : _log(log), _wifi(wifi) {}

    void tick(unsigned long now) {
        if (_inProgress) {
            _streamChunk(now);
            return;
        }
        if (_wifi.restartPending()) return;
        if (!_wifi.isConnected()) return;
        if (now >= DEADLINE) return;
        if (now - _lastCheck < CHECK_INTERVAL) return;

        _lastCheck = now;

        String latest;
        if (!_checkVersion(latest)) return;
        if (!isNewer(latest.c_str(), firmware_version)) return;

        _log.log("Nova versao encontrada: %s", latest.c_str());
        _startDownload(latest, now);
    }

    // Tags no formato "vN". Build local ("dev") aceita qualquer tag válida.
    static bool isNewer(const char* latest, const char* current) {
        long l = parseTag(latest);
        if (l < 0) return false;
        long c = parseTag(current);
        return c < 0 || l > c;
    }

    // "v42" → 42; qualquer outro formato → -1
    static long parseTag(const char* tag) {
        if (!tag || tag[0] != 'v' || tag[1] == '\0') return -1;
        long n = 0;
        for (const char* p = tag + 1; *p; p++) {
            if (*p < '0' || *p > '9' || n > 100000000L) return -1;
            n = n * 10 + (*p - '0');
        }
        return n;
    }

    bool   inProgress()    const { return _inProgress; }
    size_t bytesWritten()  const { return _written; }
    int    contentLength() const { return _contentLength; }

    // Só faz sentido com inProgress(): o download só inicia com tamanho > 0
    int progressPercent() const {
        return (int)(_written * 100UL / (size_t)_contentLength);
    }

    static constexpr unsigned long CHECK_INTERVAL = 3600000UL;  // 1 hora
    static constexpr unsigned long DEADLINE       = 82800000UL; // ~23 h
    static constexpr unsigned long STALL_TIMEOUT  = 30000UL;    // sem dados por 30 s

private:
    Logger&      _log;
    WiFiService& _wifi;

    bool             _inProgress    = false;
    HTTPClient       _http;
    WiFiClientSecure _client;
    int              _contentLength = 0;
    size_t           _written       = 0;
    unsigned long    _lastCheck     = 0;
    unsigned long    _lastProgress  = 0;
    int              _lastPct       = 0;

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
        StaticJsonDocument<JSON_OBJECT_SIZE(1)> filter;
        filter["tag_name"] = true;

        StaticJsonDocument<JSON_OBJECT_SIZE(1) + 64> doc;
        DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        http.end();

        if (err) {
            _log.log("Erro JSON versao: %s", err.c_str());
            return false;
        }

        latest = doc["tag_name"] | "";
        latest.trim();
        return latest.length() > 0;
    }

    bool _startDownload(const String& version, unsigned long now) {
        String url = "https://github.com/allanbarcelos/esp8266-dns/releases/download/";
        url += version;
        url += "/firmware.signed.bin"; // gzip + assinatura RSA

        _log.log("URL firmware: %s", url.c_str());

        _client.setInsecure();
        _http.setTimeout(10000);
        _http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

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

        _written      = 0;
        _lastPct      = 0;
        _lastProgress = now;
        _inProgress   = true;
        _wifi.setOTAInProgress(true);
        _log.log("OTA iniciado: %d bytes", _contentLength);
        return true;
    }

    void _streamChunk(unsigned long now) {
        if (!_wifi.isConnected()) {
            _abort("Wi-Fi caiu durante OTA");
            _wifi.notifyFailure();
            return;
        }
        if (now - _lastProgress > STALL_TIMEOUT) {
            _abort("OTA sem dados, abortando");
            return;
        }

        WiFiClient* stream = _http.getStreamPtr();
        size_t avail = stream->available();
        if (avail == 0) return;

        uint8_t buf[512];
        size_t len = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));

        if (Update.write(buf, len) != len) {
            _abort("Erro de escrita OTA");
            return;
        }
        _written     += len;
        _lastProgress = now;

        int pct = progressPercent();
        if (pct >= _lastPct + 10) {
            _lastPct = pct - pct % 10;
            _log.log("OTA: %d%%", _lastPct);
        }

        if (_written >= (size_t)_contentLength) {
            // end() valida tamanho, MD5 e assinatura do binário
            if (Update.end()) {
                _log.log("OTA concluido! Reiniciando...");
                _wifi.scheduleRestart(1000);
            } else {
                _log.log("Erro OTA: %s", Update.getErrorString().c_str());
            }
            _finish();
        }
    }

    void _abort(const char* reason) {
        _log.log("%s", reason);
        Update.end(false); // descarta a imagem parcial
        _finish();
    }

    void _finish() {
        _http.end();
        _inProgress = false;
        _wifi.setOTAInProgress(false);
    }
};
