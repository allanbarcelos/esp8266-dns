// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <Updater.h>
#include "Logger.h"
#include "Version.h"
#include "FlashLayout.h"
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
// O linker (ld/eagle.flash.1m64.ota.ld) deixa 400 KB antes do FS fora do
// IROM; o sketch nunca ocupa essa reserva.
//
// GitHub não negocia MFLN. O buffer TLS de 4 KB usado no Cloudflare
// não cabe o registro TLS de 16 KB: o download trava ou cai. A sessão
// com o GitHub usa 16 KB. Consulta e download compartilham um TlsClient
// e HTTP cru: HTTPClient + ArduinoJson + Location/cookies em String, em
// cima dos 16 KB de TLS, geravam OOM (Exception 29 no contexto SYS).
// O nome do header cabe em 32 bytes; o Location do asset (~1 KB) é
// malloc só durante os redirects e libertado antes do Update.begin.
// 302 fecha a sessão e abre outra no CDN (MFLN 4 KB se o CDN aceitar).
// Falha transitória retenta com backoff; se a heap não der, grava
// /ota.pending e reinicia em modo recuperação. O contador do pedido sobe
// já ao retomá-lo: um travamento (OOM no TLS) também gasta um boot.
// Imagem rejeitada por Update.end() não melhora com nova tentativa: a
// versão fica marcada em /ota.rejected (sobrevive ao reinício: sem
// heap no painel, a consulta roda em recuperação) e só uma instalação
// manual a baixa de novo.
class OTAService {
public:
    OTAService(Logger& log, WiFiService& wifi) : _log(log), _wifi(wifi) {}

    void setAutomatic(bool on) { _automatic = on; }
    bool automatic() const { return _automatic; }
    bool slim()      const { return _slim; }
    bool busy()      const {
        // _nextRetry só fica != 0 junto com _wantCheck/_wantInstall
        return _active() || _wantCheck || _wantInstall;
    }

    void begin() {
        _loadRejected();
        _loadPending();
    }

    // O POST só arma o pedido; o tick executa.
    void requestCheck() {
        if (_active() || _wantCheck) return;
        _wantInstall = false;
        _nextRetry = 0;
        _wantCheck = true;
        _note(Report::Checking, PSTR("Consultando o GitHub..."));
    }

    void requestInstall() {
        if (_active() || _available[0] == '\0') return;
        _wantCheck = false;
        _nextRetry = 0;
        _wantInstall = true;
        _note(Report::Available, PSTR("Baixando %s..."), _available);
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
        if (_net != NET_IDLE) {
            _pumpNet(now);
            return;
        }
        if (_wifi.restartPending()) return;
        if (!_wifi.isConnected()) return;

        if (_nextRetry != 0 && now < _nextRetry) return;

        if (_wantInstall) {
            _wantInstall = false;
            _nextRetry = 0;
            _download(_available, now);
            return;
        }
        if (_wantCheck) {
            _wantCheck = false;
            _nextRetry = 0;
            // Recuperação: a consulta existe para concluir o OTA, sem painel.
            _checkNow(now, _slim);
            return;
        }

        if (!_automatic) return;
        if (now >= DEADLINE) return;
        if (_lastCheck == 0) {
            if (now < FIRST_CHECK) return;
        } else if (now - _lastCheck < CHECK_INTERVAL) return;

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

    static constexpr unsigned long FIRST_CHECK    = 20000UL;    // 1ª consulta após o boot
    static constexpr unsigned long CHECK_INTERVAL = 3600000UL;  // 1 hora
    static constexpr unsigned long DEADLINE       = 82800000UL; // ~23 h
    static constexpr unsigned long STALL_TIMEOUT  = 60000UL;    // sem dados por 60 s
    static constexpr unsigned long READ_TIMEOUT   = 15000UL;    // resposta HTTP parada
    static constexpr unsigned long RETRY_MS       = 10000UL;
    static constexpr unsigned long RETRY_MAX_MS   = 20000UL;
    static constexpr uint8_t       MAX_TRIES      = 4;
    static constexpr uint8_t       MAX_BOOTS      = 3;
    static constexpr uint8_t       MAX_REDIRECTS  = 5;
    static constexpr uint16_t      GITHUB_RX      = 16384;
    static constexpr uint16_t      GITHUB_TX      = 512;
    // Exception(29)/StoreProhibited em hardware real com heap 22648/bloco
    // 22296: o guard passava, mas a alocação real do BearSSL (dentro do
    // connect(), depois da resolução DNS) ainda não cabia. 24000/19000 é a
    // margem que esta placa precisa para não entrar em connect() nesse caso.
    static constexpr uint32_t      GITHUB_HEAP    = 24000;  // 16 KB TLS + BearSSL + path
    static constexpr uint32_t      GITHUB_BLOCK   = 19000;
    static constexpr uint32_t      TLS_TAIL       = 2048;   // após o buffer RX
    static constexpr size_t        OTA_HOST_CAP   = 80;
    static constexpr size_t        OTA_PATH_CAP   = 1280;
    static constexpr const char*   PENDING_PATH   = "/ota.pending";
    static constexpr const char*   REJECTED_PATH  = "/ota.rejected";

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
    char             _rejected[16]  = {};
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

    void _note(Report report, PGM_P fmt, ...) {
        _report = report;
        va_list args;
        va_start(args, fmt);
        vsnprintf_P(_detail, sizeof(_detail), fmt, args);
        va_end(args);
    }

    bool _tlsReady() {
        uint32_t heap  = ESP.getFreeHeap();
        uint32_t block = ESP.getMaxFreeBlockSize();
        _log.log(PSTR("OTA heap %u block %u"), heap, block);
        if (heap >= GITHUB_HEAP && block >= GITHUB_BLOCK) return true;
        _note(Report::Failed, PSTR("Memoria insuficiente"));
        _log.log(PSTR("OTA: memoria insuficiente"));
        return false;
    }

    void _recoverHeap() {
        if (_available[0] == '\0') strlcpy(_available, "check", sizeof(_available));
        _persistAndReboot();
    }

    // ── Rede sem bloquear ─────────────────────────────────────────────────────
    // O connect() do TLS ainda bloqueia (BearSSL não tem handshake
    // assíncrono), mas a espera pela resposta não: cada tick consome só os
    // bytes que já chegaram e devolve o loop (painel, IoT). A resposta é
    // lida por parsers incrementais; READ_TIMEOUT sem dados encerra.

    // Cabeçalho HTTP byte a byte. location, quando não nulo, tem cap > 0.
    struct HeaderParser {
        enum Stage : uint8_t { STATUS, NAME, VALUE_WS, VALUE };
        Stage  stage;
        char   name[32];
        size_t n;
        bool   takeLoc;
        bool   takeLen;
        int    code;
        int    contentLength;
        char*  location;
        size_t locationCap;

        void begin(char* loc, size_t cap) {
            stage = STATUS;
            n = 0;
            code = 0;
            contentLength = -1;
            location = loc;
            locationCap = cap;
            if (location) location[0] = '\0';
        }

        // 1: cabeçalho completo; 0: precisa de mais; -1: resposta inválida
        int feed(char ch) {
            if (stage == VALUE_WS) {
                if (ch == ' ' || ch == '\t') return 0;
                stage = VALUE;
                n = 0;
            }
            if (stage == VALUE) {
                if (ch == '\n') {
                    if (takeLoc) location[n] = '\0';
                    if (takeLen) {
                        name[n] = '\0';
                        contentLength = atoi(name);
                    }
                    n = 0;
                    stage = NAME;
                    return 0;
                }
                if (ch == '\r') return 0;
                if (takeLoc && n + 1 < locationCap) location[n++] = ch;
                else if (takeLen && n + 1 < sizeof(name)) name[n++] = ch;
                return 0;
            }
            if (ch == '\r') return 0;
            if (stage == STATUS) {
                if (ch == '\n') {
                    name[n] = '\0';
                    if (strncmp(name, "HTTP/", 5) != 0) return -1;
                    const char* sp = strchr(name, ' ');
                    if (!sp || sp[1] < '0' || sp[1] > '9') return -1;
                    code = atoi(sp + 1);
                    stage = NAME;
                    n = 0;
                    return 0;
                }
                if (n + 1 < sizeof(name)) name[n++] = ch;
                return 0;
            }
            if (ch == '\n') {               // NAME
                if (n == 0) return 1;
                n = 0;
                return 0;
            }
            if (ch == ':') {
                name[n] = '\0';
                takeLoc = location && location[0] == '\0' && _hdrIs(name, "Location");
                takeLen = _hdrIs(name, "Content-Length");
                stage = VALUE_WS;
                return 0;
            }
            if (n + 1 < sizeof(name)) name[n++] = ch;
            return 0;
        }
    };

    // "tag_name": "<valor>" byte a byte, sem guardar o JSON
    struct TagParser {
        enum Stage : uint8_t { KEY, BEFORE_COLON, BEFORE_VALUE, VALUE };
        Stage    stage;
        unsigned ki;
        char*    tag;
        size_t   cap;
        size_t   n;

        void begin(char* t, size_t c) {
            stage = KEY;
            ki = 0;
            tag = t;
            cap = c;
            n = 0;
            tag[0] = '\0';
        }

        static bool space(char ch) {
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
        }

        // 1: tag lida; 0: precisa de mais; -1: JSON inválido
        int feed(char ch) {
            static const char key[] = "\"tag_name\"";
            if (stage == KEY) {
                if (ch == key[ki]) {
                    if (key[++ki] == '\0') stage = BEFORE_COLON;
                } else {
                    ki = (ch == key[0]) ? 1 : 0;
                }
                return 0;
            }
            if (stage == BEFORE_COLON) {
                if (space(ch)) return 0;
                if (ch != ':') return -1;
                stage = BEFORE_VALUE;
                return 0;
            }
            if (stage == BEFORE_VALUE) {
                if (space(ch)) return 0;
                if (ch != '"') return -1;
                stage = VALUE;
                return 0;
            }
            if (ch != '"') {                // VALUE
                if (n + 1 < cap) tag[n++] = ch;
                return 0;
            }
            tag[n] = '\0';
            char* s = tag;
            while (*s == ' ' || *s == '\t') s++;
            if (s != tag) memmove(tag, s, strlen(s) + 1);
            size_t len = strlen(tag);
            while (len && (tag[len - 1] == ' ' || tag[len - 1] == '\t'))
                tag[--len] = '\0';
            return 1;
        }
    };

    enum Net : uint8_t { NET_IDLE, NET_CHECK_HDR, NET_CHECK_BODY, NET_DL_HDR };

    Net           _net          = NET_IDLE;
    unsigned long _netLast      = 0;
    HeaderParser  _hdrP         = {};
    TagParser     _tagP         = {};
    char          _latest[16]   = {};
    bool          _checkInstall = false;
    char          _host[OTA_HOST_CAP] = {};
    char*         _path         = nullptr; // malloc só durante os redirects
    uint8_t       _hop          = 0;

    // Download ou sessão TLS em andamento
    bool _active() const { return _inProgress || _net != NET_IDLE; }

    // Lê o que já chegou. 1/-1: o parser da etapa concluiu ou falhou;
    // -1 também para conexão fechada ou READ_TIMEOUT sem dados; 0: aguarda.
    int _pump(unsigned long now) {
        while (_client.available() > 0) {
            char ch = (char)_client.read();
            _netLast = now;
            int r = _net == NET_CHECK_BODY ? _tagP.feed(ch) : _hdrP.feed(ch);
            if (r != 0) return r;
        }
        if (!_client.connected()) return -1;
        if (now - _netLast >= READ_TIMEOUT) return -1;
        return 0;
    }

    void _pumpNet(unsigned long now) {
        int r = _pump(now);
        if (r == 0) return;
        if (_net == NET_CHECK_HDR) _onCheckHeaders(now, r);
        else if (_net == NET_CHECK_BODY) _onCheckBody(now, r);
        else _onDownloadHeaders(now, r);
    }

    // ── consulta da versão ────────────────────────────────────────────────────

    void _checkNow(unsigned long now, bool install) {
        _checkInstall = install;
        _latest[0] = '\0';
        if (!_tlsReady()) {
            _recoverHeap();
            _checkFailed(now);
            return;
        }
        static const char host[] = "api.github.com";
        static const char path[] =
            "/repos/allanbarcelos/esp8266-dns/releases/latest";
        if (!_prepareTls(host, true)) {
            _note(Report::Failed, PSTR("Cliente TLS indisponivel"));
            _log.log(PSTR("OTA: TLS indisponivel"));
            _checkFailed(now);
            return;
        }
        if (!_client.connect(host, 443)) {
            _note(Report::Failed, PSTR("Falha ao consultar o GitHub"));
            _log.log(PSTR("Falha ao verificar versao"));
            _checkFailed(now);
            return;
        }
        _sendGet(host, path, "Accept: application/vnd.github+json");
        _hdrP.begin(nullptr, 0);
        _net = NET_CHECK_HDR;
        _netLast = now;
        _pumpNet(now);
    }

    void _checkFailed(unsigned long now) {
        _client.stop();
        _net = NET_IDLE;
        if (!_wifi.restartPending()) _scheduleRetry(now, false);
    }

    void _onCheckHeaders(unsigned long now, int r) {
        if (r < 0) {
            _note(Report::Failed, PSTR("Resposta invalida do GitHub"));
            _log.log(PSTR("Erro JSON versao: headers"));
            _checkFailed(now);
            return;
        }
        if (_hdrP.code != 200) {
            _note(Report::Failed, PSTR("GitHub HTTP %d"), _hdrP.code);
            _log.log(PSTR("Erro HTTP versao: %d"), _hdrP.code);
            _checkFailed(now);
            return;
        }
        _tagP.begin(_latest, sizeof(_latest));
        _net = NET_CHECK_BODY;
        _pumpNet(now);
    }

    void _onCheckBody(unsigned long now, int r) {
        if (r < 0) {
            _note(Report::Failed, PSTR("Resposta invalida do GitHub"));
            _log.log(PSTR("Erro JSON versao: tag_name"));
            _checkFailed(now);
            return;
        }
        if (_latest[0] == '\0') {
            _note(Report::Failed, PSTR("Versao remota vazia"));
            _checkFailed(now);
            return;
        }
        _client.stop();
        _net = NET_IDLE;
        _afterCheck(now);
    }

    void _afterCheck(unsigned long now) {
        if (!isNewer(_latest, firmware_version)) {
            bool restore = _slim;
            _available[0] = '\0';
            _tries = 0;
            _retryDelay = 0;
            _clearPending();
            _note(Report::UpToDate, PSTR("Firmware ja esta atualizado"));
            if (restore) {
                _log.log(PSTR("OTA: recuperacao concluida, restaurando painel"));
                _wifi.scheduleRestart(1500);
            }
            return;
        }
        strlcpy(_available, _latest, sizeof(_available));
        _note(Report::Available, PSTR("Nova versao %s"), _available);
        _log.log(PSTR("Nova versao encontrada: %s"), _available);
        if (!_checkInstall) return;
        if (strcmp(_available, _rejected) == 0) {
            bool restore = _slim;
            _tries = 0;
            _retryDelay = 0;
            _clearPending();
            _note(Report::Failed, PSTR("%s rejeitada; aguardando outra versao"), _available);
            _log.log(PSTR("OTA: %s ja foi rejeitada"), _available);
            if (restore) {
                _log.log(PSTR("OTA: recuperacao concluida, restaurando painel"));
                _wifi.scheduleRestart(1500);
            }
            return;
        }
        _download(_available, now);
    }

    static bool _hdrIs(const char* line, const char* name) {
        size_t n = 0;
        while (name[n]) {
            char a = line[n], b = name[n];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) return false;
            n++;
        }
        return line[n] == '\0';
    }

    // url é o próprio buffer path (Location do redirect, já limitado a
    // pathCap): o caminho só é movido para o início.
    static bool _splitUrl(const char* url, char* host, size_t hostCap,
                          char* path, size_t pathCap) {
        if (strncmp(url, "https://", 8) != 0) return false;
        const char* p = url + 8;
        const char* slash = strchr(p, '/');
        size_t hostLen = slash ? (size_t)(slash - p) : strlen(p);
        if (hostLen == 0 || hostLen >= hostCap) return false;
        memcpy(host, p, hostLen);
        host[hostLen] = '\0';
        char* colon = strchr(host, ':');
        if (colon) *colon = '\0';
        if (slash) {
            memmove(path, slash, strlen(slash) + 1);
        } else {
            strlcpy(path, "/", pathCap);
        }
        return host[0] != '\0';
    }

    static bool _isRedirect(int code) {
        return code == 301 || code == 302 || code == 303
            || code == 307 || code == 308;
    }

    bool _prepareTls(const char* host, bool tryMfln) {
        _client.stop();
        _client.setInsecure();
        uint16_t rx = GITHUB_RX;
        if (tryMfln && _client.probeMaxFragmentLength(host, 443, 4096))
            rx = 4096;
        if (!_client.prepare(15000, rx, GITHUB_TX)) return false;
        if (ESP.getMaxFreeBlockSize() < (uint32_t)rx + TLS_TAIL) {
            _client.stop();
            return false;
        }
        return true;
    }

    void _sendGet(const char* host, const char* path, const char* extraHeader = nullptr) {
        _client.print("GET ");
        _client.print(path);
        _client.print(" HTTP/1.0\r\nHost: ");
        _client.print(host);
        _client.print("\r\nUser-Agent: ESP8266-dns\r\nConnection: close\r\n");
        if (extraHeader) {
            _client.print(extraHeader);
            _client.print("\r\n");
        }
        _client.print("\r\n");
    }

    // ── início do download (redirects até o CDN) ──────────────────────────────

    void _download(const char* version, unsigned long now) {
        if (!_tlsReady()) {
            _recoverHeap();
            _downloadFailed(now);
            return;
        }
        _path = (char*)malloc(OTA_PATH_CAP);
        if (!_path) {
            _note(Report::Failed, PSTR("Memoria insuficiente"));
            _log.log(PSTR("OTA: memoria insuficiente"));
            _recoverHeap();
            _downloadFailed(now);
            return;
        }
        // Host e caminho fixos; só a tag (≤15) e o asset variam e cabem
        strlcpy(_host, "github.com", sizeof(_host));
        snprintf_P(_path, OTA_PATH_CAP,
                   PSTR("/allanbarcelos/esp8266-dns/releases/download/%s/%s"),
                   version, assetName(signingEnabled()));
        _log.log(PSTR("URL firmware: https://%s%s"), _host, _path);
        _hop = 0;
        _downloadConnect(now);
    }

    void _downloadConnect(unsigned long now) {
        if (!_prepareTls(_host, _hop > 0)) {
            _note(Report::Failed, PSTR("Cliente TLS indisponivel"));
            _log.log(PSTR("OTA: TLS indisponivel"));
            _downloadFailed(now);
            return;
        }
        if (!_client.connect(_host, 443)) {
            _note(Report::Failed, PSTR("Falha ao baixar o firmware"));
            _log.log(PSTR("Falha ao iniciar OTA"));
            _downloadFailed(now);
            return;
        }
        _sendGet(_host, _path);
        _hdrP.begin(_path, OTA_PATH_CAP); // Location reaproveita o buffer
        _net = NET_DL_HDR;
        _netLast = now;
        _pumpNet(now);
    }

    void _downloadFailed(unsigned long now) {
        _client.stop();
        _net = NET_IDLE;
        free(_path);
        _path = nullptr;
        if (!_wifi.restartPending()) _scheduleRetry(now, true);
    }

    void _onDownloadHeaders(unsigned long now, int r) {
        if (r < 0) {
            _note(Report::Failed, PSTR("Resposta OTA incompleta"));
            _log.log(PSTR("OTA: headers incompletos"));
            _downloadFailed(now);
            return;
        }
        int code = _hdrP.code;
        if (_isRedirect(code)) {
            _log.log(PSTR("OTA redirect %d"), code);
            _client.stop();
            if (_path[0] == '\0') {
                _note(Report::Failed, PSTR("Redirect OTA sem Location"));
                _log.log(PSTR("OTA: redirect sem Location"));
                _downloadFailed(now);
                return;
            }
            if (_path[0] != '/' &&
                !_splitUrl(_path, _host, sizeof(_host), _path, OTA_PATH_CAP)) {
                _note(Report::Failed, PSTR("Redirect OTA invalido"));
                _log.log(PSTR("OTA: redirect invalido"));
                _downloadFailed(now);
                return;
            }
            if (++_hop >= MAX_REDIRECTS) {
                _note(Report::Failed, PSTR("OTA excedeu redirecionamentos"));
                _log.log(PSTR("OTA: redirecionamentos esgotados"));
                _downloadFailed(now);
                return;
            }
            _downloadConnect(now);
            return;
        }
        if (code != 200) {
            _note(Report::Failed, PSTR("Download HTTP %d"), code);
            _log.log(PSTR("HTTP OTA erro: %d"), code);
            _downloadFailed(now);
            return;
        }
        _net = NET_IDLE;
        free(_path);
        _path = nullptr;

        _contentLength = _hdrP.contentLength;
        _unknownLen = _contentLength < 0;
        size_t flashSize;
        if (_unknownLen) {
            flashSize = ESP.getFreeSketchSpace();
        } else if (_contentLength == 0) {
            _note(Report::Failed, PSTR("Tamanho do firmware invalido"));
            _log.log(PSTR("Tamanho firmware invalido"));
            _downloadFailed(now);
            return;
        } else {
            flashSize = (size_t)_contentLength;
        }

        if (flashSize == 0 || !Update.begin(flashSize)) {
            _note(Report::Failed, PSTR("Sem espaco para o firmware"));
            _log.log(PSTR("Sem espaco para OTA: %s"), Update.getErrorString().c_str());
            _downloadFailed(now);
            return;
        }

        _written      = 0;
        _lastPct      = 0;
        _lastProgress = now;
        _inProgress   = true;
        _wifi.setOTAInProgress(true);
        if (_unknownLen) _log.log(PSTR("OTA iniciado: tamanho desconhecido"));
        else _log.log(PSTR("OTA iniciado: %d bytes"), _contentLength);
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

        size_t avail = (size_t)_client.available();
        if (avail == 0) {
            if (!_unknownLen) return;
            if (_client.connected()) return;
            if (_written == 0) {
                _abort("OTA vazio", now);
                return;
            }
            _complete(true);
            return;
        }

        uint8_t buf[512];
        size_t len = _client.readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));

        if (Update.write(buf, len) != len) {
            _abort("Erro de escrita OTA", now);
            return;
        }
        _written     += len;
        _lastProgress = now;

        int pct = progressPercent();
        if (pct >= _lastPct + 10) {
            _lastPct = pct - pct % 10;
            _log.log(PSTR("OTA: %d%%"), _lastPct);
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
            _log.log(PSTR("OTA concluido! Reiniciando..."));
            _note(Report::UpToDate, PSTR("Firmware atualizado"));
            _wifi.scheduleRestart(1000);
        } else {
            _note(Report::Failed, PSTR("Imagem rejeitada: %s"), Update.getErrorString().c_str());
            _log.log(PSTR("Erro OTA: %s"), Update.getErrorString().c_str());
            strlcpy(_rejected, _available, sizeof(_rejected));
            _saveRejected();
            bool restore = _slim;
            _tries = 0;
            _retryDelay = 0;
            _clearPending();
            if (restore) {
                _log.log(PSTR("OTA: recuperacao encerrada, restaurando painel"));
                _wifi.scheduleRestart(1500);
            }
        }
        _finish();
    }

    void _abort(const char* reason, unsigned long now) {
        _note(Report::Failed, PSTR("%s"), reason);
        _log.log(PSTR("%s"), reason);
        Update.end(false);
        _finish();
        _scheduleRetry(now, true);
    }

    void _finish() {
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
            if (install) { // instalação só é pedida com _available preenchido
                _persistAndReboot();
                return;
            }
            _note(Report::Failed, PSTR("OTA falhou, tenta de novo mais tarde"));
            _log.log(PSTR("OTA: tentativas esgotadas"));
            return;
        }
        unsigned long delayMs = _retryDelay == 0 ? RETRY_MS : _retryDelay * 2;
        if (delayMs > RETRY_MAX_MS) delayMs = RETRY_MAX_MS;
        _retryDelay = delayMs;
        _nextRetry = now + delayMs;
        if (install) _wantInstall = true;
        else _wantCheck = true;
        _note(Report::Failed, PSTR("Nova tentativa em %u s"), (unsigned)(delayMs / 1000));
        _log.log(PSTR("OTA: nova tentativa em %u s"), (unsigned)(delayMs / 1000));
    }

    void _persistAndReboot() {
        uint8_t boots = (uint8_t)(_pendingBoots + 1);
        if (boots > MAX_BOOTS) {
            _clearPending();
            _wantInstall = false;
            _wantCheck = false;
            _note(Report::Failed, PSTR("OTA desistiu apos recuperacao"));
            _log.log(PSTR("OTA: desistiu apos recuperacao"));
            _wifi.scheduleRestart(1500);
            return;
        }
        if (!_savePending(_available, boots)) _log.log(PSTR("OTA: nao gravou pedido de recuperacao"));
        else _log.log(PSTR("OTA: recuperacao no proximo boot"));
        _wifi.scheduleRestart(1500);
    }

    bool _savePending(const char* tag, uint8_t boots) {
        File f = LittleFS.open(PENDING_PATH, "w");
        if (!f) return false;
        char line[24];
        snprintf(line, sizeof(line), "%s %u", tag, (unsigned)boots);
        f.write((const uint8_t*)line, strlen(line));
        f.close();
        return true;
    }

    void _saveRejected() {
        File f = LittleFS.open(REJECTED_PATH, "w");
        if (!f) {
            _log.log(PSTR("OTA: nao gravou versao rejeitada"));
            return;
        }
        f.write((const uint8_t*)_rejected, strlen(_rejected));
        f.close();
    }

    void _loadRejected() {
        File f = LittleFS.open(REJECTED_PATH, "r");
        if (!f) return;
        size_t n = f.readBytes(_rejected, sizeof(_rejected) - 1);
        f.close();
        _rejected[n] = '\0';
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
            _log.log(PSTR("OTA: pedido de recuperacao invalido"));
            return;
        }
        if (boots > MAX_BOOTS) {
            LittleFS.remove(PENDING_PATH);
            _log.log(PSTR("OTA: recuperacao esgotada"));
            return;
        }
        _pendingBoots = (uint8_t)boots;
        _slim = true;
        // Antes de tentar: se este boot travar, o próximo já conta mais um.
        // Sem gravar, a recuperação segue com o contador do último pedido.
        (void)_savePending(tag, (uint8_t)(boots + 1));
        if (isCheck) {
            _available[0] = '\0';
            _wantCheck = true;
            _note(Report::Checking, PSTR("Retomando consulta"));
            _log.log(PSTR("OTA: retomando consulta (boot %u)"), (unsigned)_pendingBoots);
            return;
        }
        strlcpy(_available, tag, sizeof(_available));
        _wantInstall = true;
        _note(Report::Available, PSTR("Retomando %s"), _available);
        _log.log(PSTR("OTA: retomando %s (boot %u)"), _available, (unsigned)_pendingBoots);
    }
};
