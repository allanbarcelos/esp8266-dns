#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <LittleFS.h>
#include <cstdarg>
#include <Updater.h>
#include <ArduinoJson.h>
#include "Logger.h"
#include "Version.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"

// Atualização OTA a partir dos GitHub Releases.
//
// O transporte usa TLS sem validação de certificado. O arquivo baixado
// depende de o build embutir public.key:
//   - com a chave, firmware.signed.bin; Update.end() confere a assinatura
//     RSA-2048/SHA-256 e grava só o gzip;
//   - sem a chave, firmware.bin (gzip puro).
// O eboot lê o tamanho descomprimido nos 4 últimos bytes. No arquivo
// assinado esses bytes são o tamanho da assinatura. Gravá-lo sem remover
// o trailer faz o bootloader copiar a imagem errada. Versões mais antigas
// que a atual são ignoradas, impedindo downgrade.
//
// A imagem é comprimida com gzip (o eboot descomprime no boot): sem isso,
// atual + nova não cabem juntas nos ~940 KB de área de sketch de 1 MB.
//
// GitHub não negocia MFLN. O buffer TLS de 4 KB usado no Cloudflare
// não cabe o registro TLS de 16 KB: o download trava ou cai. A sessão
// com o GitHub usa 16 KB. Falha transitória retenta com backoff; se
// a heap não der, grava /ota.pending e reinicia em modo recuperação.
class OTAService {
public:
    OTAService(Logger& log, WiFiService& wifi) : _log(log), _wifi(wifi) {}

    void setAutomatic(bool on) { _automatic = on; }
    bool automatic() const { return _automatic; }
    bool slim()      const { return _slim; }
    bool busy()      const {
        return _inProgress || _wantCheck || _wantInstall || _nextRetry != 0;
    }

    void begin() { _loadPending(); }

    // A consulta bloqueia o loop (TLS). O POST só arma o pedido; o tick executa.
    void requestCheck() {
        if (_inProgress || _wantCheck) return;
        _wantInstall = false;
        _nextRetry = 0;
        _wantCheck = true;
        _note(Report::Checking, "Consultando o GitHub...");
    }

    void requestInstall() {
        if (_inProgress || _available[0] == '\0') return;
        _wantCheck = false;
        _nextRetry = 0;
        _wantInstall = true;
        _note(Report::Available, "Baixando %s...", _available);
    }

    enum class Report : uint8_t { None, Checking, UpToDate, Available, Failed };

    Report      report()           const { return _report; }
    const char* detail()           const { return _detail; }
    const char* availableVersion() const { return _available; }
    bool        pending()          const { return busy(); }

    void tick(unsigned long now) {
        if (_inProgress) {
            _streamChunk(now);
            return;
        }
        if (_wifi.restartPending()) return;
        if (!_wifi.isConnected()) return;

        if (_nextRetry != 0 && now < _nextRetry) return;

        if (_wantInstall) {
            _wantInstall = false;
            _nextRetry = 0;
            if (!_startDownload(_available, now) && !_wifi.restartPending())
                _scheduleRetry(now, true);
            return;
        }
        if (_wantCheck) {
            _wantCheck = false;
            _nextRetry = 0;
            _checkNow(now, false);
            return;
        }

        if (!_automatic) return;
        if (now >= DEADLINE) return;
        if (now - _lastCheck < CHECK_INTERVAL) return;

        _lastCheck = now;
        _nextRetry = 0;
        _checkNow(now, true);
    }

    // Tags no formato "vN". Build local ("dev") aceita qualquer tag válida.
    static bool isNewer(const char* latest, const char* current) {
        long l = parseTag(latest);
        if (l < 0) return false;
        long c = parseTag(current);
        return c < 0 || l > c;
    }

    // Com public.key o Updater remove a assinatura. Sem ela, o gzip puro
    // é o único arquivo cujo final o eboot interpreta como tamanho.
    // ARDUINO_SIGNING vem do build. Não incluir Updater_Signing.h: o do
    // core redefine a macro como 0 e apagaria a escolha do asset.
    static const char* assetName(bool signing) {
        return signing ? "firmware.signed.bin" : "firmware.bin";
    }

    static bool signingEnabled() {
#if ARDUINO_SIGNING
        return true;
#else
        return false;
#endif
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

    int progressPercent() const {
        if (_contentLength <= 0) return 0;
        return (int)(_written * 100UL / (size_t)_contentLength);
    }

    static constexpr unsigned long CHECK_INTERVAL = 3600000UL;  // 1 hora
    static constexpr unsigned long DEADLINE       = 82800000UL; // ~23 h
    static constexpr unsigned long STALL_TIMEOUT  = 60000UL;    // sem dados por 60 s
    static constexpr unsigned long RETRY_MS       = 10000UL;
    static constexpr unsigned long RETRY_MAX_MS   = 20000UL;
    static constexpr uint8_t       MAX_TRIES      = 4;
    static constexpr uint8_t       MAX_BOOTS      = 3;
    static constexpr uint16_t      GITHUB_RX      = 16384;
    static constexpr uint16_t      GITHUB_TX      = 512;
    static constexpr uint32_t      GITHUB_HEAP    = 24000;
    static constexpr uint32_t      GITHUB_BLOCK   = 19000;
    static constexpr const char*   PENDING_PATH   = "/ota.pending";

private:
    Logger&      _log;
    WiFiService& _wifi;

    bool             _inProgress    = false;
    bool             _automatic     = true;
    bool             _wantCheck     = false;
    bool             _wantInstall   = false;
    bool             _slim          = false;
    bool             _unknownLen    = false;
    Report           _report        = Report::None;
    char             _detail[80]    = {};
    char             _available[16] = {};
    HTTPClient       _http;
    TlsClient        _client;
    int              _contentLength = 0;
    size_t           _written       = 0;
    unsigned long    _lastCheck     = 0;
    unsigned long    _lastProgress  = 0;
    unsigned long    _nextRetry     = 0;
    unsigned long    _retryDelay    = 0;
    int              _lastPct       = 0;
    uint8_t          _tries         = 0;
    uint8_t          _pendingBoots  = 0;

    void _note(Report report, const char* fmt, ...) {
        _report = report;
        va_list args;
        va_start(args, fmt);
        vsnprintf(_detail, sizeof(_detail), fmt, args);
        va_end(args);
    }

    bool _tlsReady() {
        uint32_t heap  = ESP.getFreeHeap();
        uint32_t block = ESP.getMaxFreeBlockSize();
        uint32_t needHeap  = _slim ? 20000u : GITHUB_HEAP;
        uint32_t needBlock = _slim ? 17000u : GITHUB_BLOCK;
        _log.log("OTA heap %u block %u", heap, block);
        if (heap >= needHeap && block >= needBlock) return true;
        _note(Report::Failed, "Memoria insuficiente");
        _log.log("OTA: memoria insuficiente");
        return false;
    }

    void _recoverHeap() {
        if (_available[0] == '\0') strlcpy(_available, "check", sizeof(_available));
        _persistAndReboot();
    }

    void _checkNow(unsigned long now, bool install) {
        String latest;
        if (!_checkVersion(latest)) {
            if (!_wifi.restartPending()) _scheduleRetry(now, false);
            return;
        }
        if (!isNewer(latest.c_str(), firmware_version)) {
            _available[0] = '\0';
            _tries = 0;
            _retryDelay = 0;
            _clearPending();
            _note(Report::UpToDate, "Firmware ja esta atualizado");
            return;
        }
        strlcpy(_available, latest.c_str(), sizeof(_available));
        _note(Report::Available, "Nova versao %s", _available);
        _log.log("Nova versao encontrada: %s", _available);
        if (!install) return;
        if (!_startDownload(_available, now) && !_wifi.restartPending())
            _scheduleRetry(now, true);
    }

    // Consulta a GitHub Releases API para obter o tag_name do último release
    bool _checkVersion(String& latest) {
        if (!_tlsReady()) {
            _recoverHeap();
            return false;
        }

        TlsClient client;
        client.setInsecure();
        if (!client.prepare(8000, GITHUB_RX, GITHUB_TX)) {
            _note(Report::Failed, "Cliente TLS indisponivel");
            _log.log("OTA: TLS indisponivel");
            return false;
        }
        HTTPClient http;
        http.setTimeout(8000);
        http.setUserAgent("ESP8266-dns");
        http.useHTTP10(true);

        if (!http.begin(client, "https://api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest")) {
            _note(Report::Failed, "Falha ao consultar o GitHub");
            _log.log("Falha ao verificar versao");
            return false;
        }
        http.addHeader("Accept", "application/vnd.github+json");

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
        if (!_tlsReady()) {
            _recoverHeap();
            return false;
        }
        _client.stop();
        _client.setInsecure();
        if (!_client.prepare(15000, GITHUB_RX, GITHUB_TX)) {
            _note(Report::Failed, "Cliente TLS indisponivel");
            _log.log("OTA: TLS indisponivel");
            return false;
        }

        String url = "https://github.com/allanbarcelos/esp8266-dns/releases/download/";
        url += version;
        url += "/";
        url += assetName(signingEnabled());

        _log.log("URL firmware: %s", url.c_str());

        _http.setTimeout(15000);
        _http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        _http.setRedirectLimit(5);
        _http.useHTTP10(true);
        _http.setUserAgent("ESP8266-dns");

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
        _unknownLen = _contentLength < 0;
        size_t flashSize;
        if (_unknownLen) {
            flashSize = ESP.getFreeSketchSpace();
        } else if (_contentLength == 0) {
            _note(Report::Failed, "Tamanho do firmware invalido");
            _log.log("Tamanho firmware invalido");
            _http.end();
            return false;
        } else {
            flashSize = (size_t)_contentLength;
        }

        if (flashSize == 0 || !Update.begin(flashSize)) {
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
        if (_unknownLen) _log.log("OTA iniciado: tamanho desconhecido");
        else _log.log("OTA iniciado: %d bytes", _contentLength);
        return true;
    }

    void _streamChunk(unsigned long now) {
        if (!_wifi.isConnected()) {
            _abort("Wi-Fi caiu durante OTA", now);
            return;
        }
        if (now - _lastProgress > STALL_TIMEOUT) {
            _abort("OTA sem dados, abortando", now);
            return;
        }

        WiFiClient* stream = _http.getStreamPtr();
        size_t avail = stream->available();
        if (avail == 0) {
            if (!_unknownLen) return;
            if (_http.connected()) return;
            if (_written == 0) {
                _abort("OTA vazio", now);
                return;
            }
            _complete(true);
            return;
        }

        uint8_t buf[512];
        size_t len = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));

        if (Update.write(buf, len) != len) {
            _abort("Erro de escrita OTA", now);
            return;
        }
        _written     += len;
        _lastProgress = now;

        int pct = progressPercent();
        if (pct >= _lastPct + 10) {
            _lastPct = pct - pct % 10;
            _log.log("OTA: %d%%", _lastPct);
        }

        if (!_unknownLen && _written >= (size_t)_contentLength) {
            _complete(false);
        }
    }

    void _complete(bool evenIfRemaining) {
        if (Update.end(evenIfRemaining)) {
            _tries = 0;
            _retryDelay = 0;
            _clearPending();
            _log.log("OTA concluido! Reiniciando...");
            _note(Report::UpToDate, "Firmware atualizado");
            _wifi.scheduleRestart(1000);
        } else {
            _note(Report::Failed, "Imagem rejeitada: %s", Update.getErrorString().c_str());
            _log.log("Erro OTA: %s", Update.getErrorString().c_str());
        }
        _finish();
    }

    void _abort(const char* reason, unsigned long now) {
        _note(Report::Failed, "%s", reason);
        _log.log("%s", reason);
        Update.end(false);
        _finish();
        _scheduleRetry(now, true);
    }

    void _finish() {
        _http.end();
        _client.stop();
        _inProgress = false;
        _unknownLen = false;
        _wifi.setOTAInProgress(false);
    }

    void _scheduleRetry(unsigned long now, bool install) {
        _tries++;
        if (_tries >= MAX_TRIES) {
            _tries = 0;
            _retryDelay = 0;
            _nextRetry = 0;
            if (install && _available[0] != '\0') {
                _persistAndReboot();
                return;
            }
            _note(Report::Failed, "OTA falhou, tenta de novo mais tarde");
            _log.log("OTA: tentativas esgotadas");
            return;
        }
        unsigned long delayMs = _retryDelay == 0 ? RETRY_MS : _retryDelay * 2;
        if (delayMs > RETRY_MAX_MS) delayMs = RETRY_MAX_MS;
        _retryDelay = delayMs;
        _nextRetry = now + delayMs;
        if (install) _wantInstall = true;
        else _wantCheck = true;
        _note(Report::Failed, "Nova tentativa em %u s", (unsigned)(delayMs / 1000));
        _log.log("OTA: nova tentativa em %u s", (unsigned)(delayMs / 1000));
    }

    void _persistAndReboot() {
        uint8_t boots = (uint8_t)(_pendingBoots + 1);
        if (boots > MAX_BOOTS) {
            _clearPending();
            _wantInstall = false;
            _wantCheck = false;
            _note(Report::Failed, "OTA desistiu apos recuperacao");
            _log.log("OTA: desistiu apos recuperacao");
            _wifi.scheduleRestart(1500);
            return;
        }
        if (!_savePending(boots)) _log.log("OTA: nao gravou pedido de recuperacao");
        else _log.log("OTA: recuperacao no proximo boot");
        _wifi.scheduleRestart(1500);
    }

    bool _savePending(uint8_t boots) {
        File f = LittleFS.open(PENDING_PATH, "w");
        if (!f) return false;
        char line[24];
        snprintf(line, sizeof(line), "%s %u", _available, (unsigned)boots);
        f.write((const uint8_t*)line, strlen(line));
        f.close();
        return true;
    }

    void _clearPending() {
        if (LittleFS.exists(PENDING_PATH)) LittleFS.remove(PENDING_PATH);
        _pendingBoots = 0;
        _slim = false;
    }

    void _loadPending() {
        if (!LittleFS.exists(PENDING_PATH)) return;
        File f = LittleFS.open(PENDING_PATH, "r");
        if (!f) {
            LittleFS.remove(PENDING_PATH);
            return;
        }
        char buf[24];
        size_t n = f.readBytes(buf, sizeof(buf) - 1);
        f.close();
        buf[n] = '\0';
        char tag[16];
        unsigned boots = 0;
        int got = sscanf(buf, "%15s %u", tag, &boots);
        bool isCheck = strcmp(tag, "check") == 0;
        if (got < 1 || (!isCheck && parseTag(tag) < 0)) {
            LittleFS.remove(PENDING_PATH);
            _log.log("OTA: pedido de recuperacao invalido");
            return;
        }
        if (boots > MAX_BOOTS) {
            LittleFS.remove(PENDING_PATH);
            _log.log("OTA: recuperacao esgotada");
            return;
        }
        _pendingBoots = (uint8_t)boots;
        _slim = true;
        if (isCheck) {
            _available[0] = '\0';
            _wantCheck = true;
            _note(Report::Checking, "Retomando consulta");
            _log.log("OTA: retomando consulta (boot %u)", (unsigned)_pendingBoots);
            return;
        }
        strlcpy(_available, tag, sizeof(_available));
        _wantInstall = true;
        _note(Report::Available, "Retomando %s", _available);
        _log.log("OTA: retomando %s (boot %u)", _available, (unsigned)_pendingBoots);
    }
};
