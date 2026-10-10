#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <cstdarg>
#include <Updater.h>
#include <ArduinoJson.h>
#include "Logger.h"
#include "Version.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"

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

    void setAutomatic(bool on) { _automatic = on; }
    bool automatic() const { return _automatic; }

    // A consulta bloqueia o loop (TLS). O POST só arma o pedido; o tick executa.
    void requestCheck() {
        if (_inProgress || _wantCheck) return;
        _wantInstall = false;
        _wantCheck = true;
        _note(Report::Checking, "Consultando o GitHub...");
    }

    void requestInstall() {
        if (_inProgress || _available[0] == '\0') return;
        _wantCheck = false;
        _wantInstall = true;
        _note(Report::Available, "Baixando %s...", _available);
    }

    enum class Report : uint8_t { None, Checking, UpToDate, Available, Failed };

    Report      report()           const { return _report; }
    const char* detail()           const { return _detail; }
    const char* availableVersion() const { return _available; }
    bool        pending()          const { return _wantCheck || _wantInstall || _inProgress; }

    void tick(unsigned long now) {
        if (_inProgress) {
            _streamChunk(now);
            return;
        }
        if (_wifi.restartPending()) return;

        if (!_wifi.isConnected()) {
            if (_wantCheck || _wantInstall) {
                _wantCheck = false;
                _wantInstall = false;
                _note(Report::Failed, "Wi-Fi desconectado");
                _log.log("OTA adiado: Wi-Fi desconectado");
            }
            return;
        }

        if (_wantInstall) {
            _wantInstall = false;
            _startDownload(_available, now);
            return;
        }
        if (_wantCheck) {
            _wantCheck = false;
            _checkNow(now, false);
            return;
        }

        if (!_automatic) return;
        if (now >= DEADLINE) return;
        if (now - _lastCheck < CHECK_INTERVAL) return;

        _lastCheck = now;
        _checkNow(now, true);
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
    bool             _automatic     = true;
    bool             _wantCheck     = false;
    bool             _wantInstall   = false;
    Report           _report        = Report::None;
    char             _detail[80]    = {};
    char             _available[16] = {};
    HTTPClient       _http;
    TlsClient        _client;
    int              _contentLength = 0;
    size_t           _written       = 0;
    unsigned long    _lastCheck     = 0;
    unsigned long    _lastProgress  = 0;
    int              _lastPct       = 0;

    void _note(Report report, const char* fmt, ...) {
        _report = report;
        va_list args;
        va_start(args, fmt);
        vsnprintf(_detail, sizeof(_detail), fmt, args);
        va_end(args);
    }

    bool _tlsReady() {
        if (PublicIPResolver::memoryOk()) return true;
        _note(Report::Failed, "Memoria insuficiente");
        _log.log("OTA: memoria insuficiente");
        return false;
    }

    void _checkNow(unsigned long now, bool install) {
        String latest;
        if (!_checkVersion(latest)) return;
        if (!isNewer(latest.c_str(), firmware_version)) {
            _available[0] = '\0';
            _note(Report::UpToDate, "Firmware ja esta atualizado");
            return;
        }
        strlcpy(_available, latest.c_str(), sizeof(_available));
        _note(Report::Available, "Nova versao %s", _available);
        _log.log("Nova versao encontrada: %s", _available);
        if (install) _startDownload(_available, now);
    }

    // Consulta a GitHub Releases API para obter o tag_name do último release
    bool _checkVersion(String& latest) {
        if (!_tlsReady()) return false;

        TlsClient client;
        client.setInsecure();
        if (!client.prepare(5000)) {
            _note(Report::Failed, "Cliente TLS indisponivel");
            _log.log("OTA: TLS indisponivel");
            return false;
        }
        HTTPClient http;
        http.setTimeout(5000);

        if (!http.begin(client, "https://api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest")) {
            _note(Report::Failed, "Falha ao consultar o GitHub");
            _log.log("Falha ao verificar versao");
            return false;
        }
        http.addHeader("Accept", "application/vnd.github+json");
        http.addHeader("User-Agent", "ESP8266");

        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            _note(Report::Failed, "GitHub HTTP %d", code);
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
            _note(Report::Failed, "Resposta invalida do GitHub");
            _log.log("Erro JSON versao: %s", err.c_str());
            return false;
        }

        latest = doc["tag_name"] | "";
        latest.trim();
        if (latest.length() == 0) {
            _note(Report::Failed, "Versao remota vazia");
            return false;
        }
        return true;
    }

    bool _startDownload(const char* version, unsigned long now) {
        if (!_tlsReady()) return false;
        _client.setInsecure();
        if (!_client.prepare(10000)) {
            _note(Report::Failed, "Cliente TLS indisponivel");
            _log.log("OTA: TLS indisponivel");
            return false;
        }

        String url = "https://github.com/allanbarcelos/esp8266-dns/releases/download/";
        url += version;
        url += "/firmware.signed.bin"; // gzip + assinatura RSA

        _log.log("URL firmware: %s", url.c_str());

        _http.setTimeout(10000);
        _http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

        if (!_http.begin(_client, url)) {
            _note(Report::Failed, "Falha ao baixar o firmware");
            _log.log("Falha ao iniciar OTA");
            return false;
        }

        int code = _http.GET();
        if (code != HTTP_CODE_OK) {
            _note(Report::Failed, "Download HTTP %d", code);
            _log.log("HTTP OTA erro: %d", code);
            _http.end();
            return false;
        }

        _contentLength = _http.getSize();
        if (_contentLength <= 0) {
            _note(Report::Failed, "Tamanho do firmware invalido");
            _log.log("Tamanho firmware invalido");
            _http.end();
            return false;
        }

        if (!Update.begin(_contentLength)) {
            _note(Report::Failed, "Sem espaco para o firmware");
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
                _note(Report::Failed, "Imagem rejeitada: %s", Update.getErrorString().c_str());
                _log.log("Erro OTA: %s", Update.getErrorString().c_str());
            }
            _finish();
        }
    }

    void _abort(const char* reason) {
        _note(Report::Failed, "%s", reason);
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
