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
#include "TlsClient.h"

// The image's SHA-256 bound to the version it was downloaded as: the
// signature covers SHA-256(SHA-256(image) || tag), not the image alone.
// Over an unvalidated connection, someone in the middle could otherwise
// answer "v99" and hand over an older release, genuinely signed, and the
// device would install it. The second round reuses the same hash object
// (the core's SHA-256), so this adds no crypto code.
class TaggedHash : public UpdaterHashClass {
public:
    explicit TaggedHash(UpdaterHashClass& inner) : _inner(inner) {}

    void tag(const char* t) { strlcpy(_tag, t, sizeof(_tag)); }

    void begin() override { _inner.begin(); }
    void add(const void* data, uint32_t len) override { _inner.add(data, len); }
    void end() override {
        _inner.end();
        uint8_t image[32];   // SHA-256
        memcpy(image, _inner.hash(), sizeof(image));
        _inner.begin();
        _inner.add(image, sizeof(image));
        _inner.add(_tag, (uint32_t)strlen(_tag));
        _inner.end();
    }
    int len() override { return _inner.len(); }
    const void* hash() override { return _inner.hash(); }
    const unsigned char* oid() override { return _inner.oid(); }

private:
    UpdaterHashClass& _inner;
    char              _tag[16] = {};
};

#if ARDUINO_SIGNING
// The core's SHA-256 and RSA verifier with public.key (BearSSLHelpers.cpp)
namespace esp8266 {
extern UpdaterHashClass& updaterSigningHash;
extern UpdaterVerifyClass& updaterSigningVerifier;
}
#endif

// OTA update from GitHub Releases.
//
// The transport uses TLS without certificate validation. The downloaded file
// depends on whether the build embeds public.key:
//   - with the key, firmware.tagged.bin; Update.end() checks the
//     RSA-2048/SHA-256 signature over the image and its tag (TaggedHash)
//     and writes only the gzip. firmware.signed.bin, signed over the image
//     alone, stays in each release for devices from before the tag;
//   - without the key, firmware.bin (plain gzip).
// eboot reads the uncompressed size from the last 4 bytes. In the signed
// file those bytes are the signature length. Writing it without removing the
// trailer makes the bootloader copy the wrong image. Versions older than the
// current one are ignored, which prevents downgrades.
//
// The image is gzip-compressed (eboot decompresses it at boot): without
// that, current + new do not fit together in the ~940 KB sketch area of 1 MB.
// The linker (ld/eagle.flash.1m64.ota.ld) leaves 400 KB before the FS
// outside IROM; the sketch never takes that reserve.
//
// GitHub does not negotiate MFLN. The 4 KB TLS buffer used for Cloudflare
// cannot hold a 16 KB TLS record: the download stalls or drops. The GitHub
// session uses 16 KB. The check and the download share one TlsClient and raw
// HTTP: HTTPClient + ArduinoJson + Location/cookies in String, on top of the
// 16 KB of TLS, caused OOM (Exception 29 in the SYS context). The header
// name fits in 32 bytes; the asset Location (~1 KB) is malloc'ed only during
// the redirects and freed before Update.begin. A 302 closes the session and
// opens another one at the CDN (MFLN 4 KB if the CDN accepts it). A
// transient failure retries with backoff; if the heap is short, it writes
// /ota.pending and restarts in recovery mode. The request counter goes up as
// soon as it resumes: a crash (OOM in TLS) also spends a boot. An image
// rejected by Update.end() does not get better on retry: the version is
// marked in /ota.rejected (it survives the restart: without heap for the
// panel, the check runs in recovery) and only a manual install downloads it
// again.
class OTAService {
public:
    OTAService(Logger& log, WiFiService& wifi) : _log(log), _wifi(wifi) {}

    void setAutomatic(bool on) { _automatic = on; }
    bool automatic() const { return _automatic; }
    bool slim()      const { return _slim; }
    bool online()    const { return _active(); }   // a check or download on the network
    bool busy()      const {
        // _nextRetry is only != 0 together with _wantCheck/_wantInstall
        return _active() || _wantCheck || _wantInstall;
    }

    // Check or download now or on the next tick (ntfy releases TLS)
    bool wantsNet(unsigned long now) const {
        if (busy()) return true;
        if (!_automatic || now >= DEADLINE) return false;
        if (_lastCheck == 0) return now >= FIRST_CHECK;
        return now - _lastCheck >= CHECK_INTERVAL;
    }

    void begin() {
#if ARDUINO_SIGNING
        Update.installSignature(&_tagged, &esp8266::updaterSigningVerifier);
#endif
        _loadRejected();
        _loadPending();
    }

    // The POST only arms the request; the tick runs it.
    void requestCheck() {
        if (_active() || _wantCheck) return;
        _wantInstall = false;
        _nextRetry = 0;
        _wantCheck = true;
        _note(Report::Checking, PSTR("Checking GitHub..."));
    }

    void requestInstall() {
        if (_active() || _available[0] == '\0') return;
        _wantCheck = false;
        _nextRetry = 0;
        _wantInstall = true;
        _note(Report::Available, PSTR("Downloading %s..."), _available);
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
            // Recovery: the check exists to finish the OTA, with no panel.
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

    // Tags in the "vN" format. A local build ("dev") accepts any valid tag.
    static bool isNewer(const char* latest, const char* current) {
        long l = parseTag(latest);
        if (l < 0) return false;
        long c = parseTag(current);
        return c < 0 || l > c;
    }

    // With public.key the Updater strips the signature. Without it, the
    // plain gzip is the only file whose ending eboot reads as the size.
    // ARDUINO_SIGNING comes from the build. Do not include Updater_Signing.h:
    // the core's redefines the macro as 0 and would erase the asset choice.
    static const char* assetName(bool signing) {
        return signing ? "firmware.tagged.bin" : "firmware.bin";
    }

    static bool signingEnabled() {
#if ARDUINO_SIGNING
        return true;
#else
        return false;
#endif
    }

    // "v42" → 42; any other format → -1
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

    static constexpr unsigned long FIRST_CHECK    = 20000UL;    // first check after boot
    static constexpr unsigned long CHECK_INTERVAL = 3600000UL;  // 1 hour
    static constexpr unsigned long DEADLINE       = 82800000UL; // ~23 h
    static constexpr unsigned long STALL_TIMEOUT  = 60000UL;    // no data for 60 s
    static constexpr unsigned long READ_TIMEOUT   = 15000UL;    // HTTP response stalled
    static constexpr unsigned long RETRY_MS       = 10000UL;
    static constexpr unsigned long RETRY_MAX_MS   = 20000UL;
    static constexpr uint8_t       MAX_TRIES      = 4;
    static constexpr uint8_t       MAX_BOOTS      = 3;
    static constexpr uint8_t       MAX_REDIRECTS  = 5;
    static constexpr uint16_t      GITHUB_RX      = 16384;
    static constexpr uint16_t      GITHUB_TX      = 512;
    // Exception(29)/StoreProhibited on real hardware with heap 22648/block
    // 22296: the guard passed, but BearSSL's real allocation (inside
    // connect(), after DNS resolution) still did not fit. 24000/19000 is the
    // margin this board needs so it does not enter connect() in that case.
    static constexpr uint32_t      GITHUB_HEAP    = 24000;  // 16 KB TLS + BearSSL + path
    static constexpr uint32_t      GITHUB_BLOCK   = 19000;
    static constexpr uint32_t      TLS_TAIL       = 2048;   // after the RX buffer
    static constexpr size_t        OTA_HOST_CAP   = 80;
    static constexpr size_t        OTA_PATH_CAP   = 1280;
    static constexpr const char*   PENDING_PATH   = "/ota.pending";
    static constexpr const char*   REJECTED_PATH  = "/ota.rejected";

private:
    Logger&      _log;
    WiFiService& _wifi;
#if ARDUINO_SIGNING
    TaggedHash   _tagged{esp8266::updaterSigningHash};
#endif

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
        _note(Report::Failed, PSTR("Not enough memory"));
        _log.log(PSTR("OTA: not enough memory"));
        return false;
    }

    void _recoverHeap() {
        if (_available[0] == '\0') strlcpy(_available, "check", sizeof(_available));
        _persistAndReboot();
    }

    // ── Non-blocking network ──────────────────────────────────────────────────
    // The TLS connect() still blocks (BearSSL has no asynchronous
    // handshake), but waiting for the response does not: each tick consumes
    // only the bytes that already arrived and hands back the loop (panel,
    // IoT). The response is read by incremental parsers; READ_TIMEOUT with no
    // data ends it.

    // HTTP header byte by byte. location, when not null, has cap > 0.
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

        // 1: header complete; 0: needs more; -1: invalid response
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

    // "tag_name": "<value>" byte by byte, without keeping the JSON
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

        // 1: tag read; 0: needs more; -1: invalid JSON
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
    char*         _path         = nullptr; // malloc'ed only during the redirects
    uint8_t       _hop          = 0;

    // Download or TLS session in progress
    bool _active() const { return _inProgress || _net != NET_IDLE; }

    // Reads what already arrived. 1/-1: the step's parser finished or failed;
    // -1 also for a closed connection or READ_TIMEOUT with no data; 0: waits.
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

    // ── version check ─────────────────────────────────────────────────────────

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
            _note(Report::Failed, PSTR("TLS client unavailable"));
            _log.log(PSTR("OTA: TLS unavailable"));
            _checkFailed(now);
            return;
        }
        if (!_client.connect(host, 443)) {
            _note(Report::Failed, PSTR("Could not reach GitHub"));
            _log.log(PSTR("Version check failed"));
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
            _note(Report::Failed, PSTR("Invalid response from GitHub"));
            _log.log(PSTR("Version JSON error: headers"));
            _checkFailed(now);
            return;
        }
        if (_hdrP.code != 200) {
            _note(Report::Failed, PSTR("GitHub HTTP %d"), _hdrP.code);
            _log.log(PSTR("Version HTTP error: %d"), _hdrP.code);
            _checkFailed(now);
            return;
        }
        _tagP.begin(_latest, sizeof(_latest));
        _net = NET_CHECK_BODY;
        _pumpNet(now);
    }

    void _onCheckBody(unsigned long now, int r) {
        if (r < 0) {
            _note(Report::Failed, PSTR("Invalid response from GitHub"));
            _log.log(PSTR("Version JSON error: tag_name"));
            _checkFailed(now);
            return;
        }
        if (_latest[0] == '\0') {
            _note(Report::Failed, PSTR("Empty remote version"));
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
            _note(Report::UpToDate, PSTR("Firmware is up to date"));
            if (restore) {
                _log.log(PSTR("OTA: recovery done, restoring the panel"));
                _wifi.scheduleRestart(1500);
            }
            return;
        }
        strlcpy(_available, _latest, sizeof(_available));
        _note(Report::Available, PSTR("New version %s"), _available);
        _log.log(PSTR("New version found: %s"), _available);
        if (!_checkInstall) return;
        if (strcmp(_available, _rejected) == 0) {
            bool restore = _slim;
            _tries = 0;
            _retryDelay = 0;
            _clearPending();
            _note(Report::Failed, PSTR("%s rejected; waiting for another version"), _available);
            _log.log(PSTR("OTA: %s was already rejected"), _available);
            if (restore) {
                _log.log(PSTR("OTA: recovery done, restoring the panel"));
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

    // url is the path buffer itself (the redirect Location, already limited
    // to pathCap): the path is only moved to the start.
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

    // ── download start (redirects up to the CDN) ──────────────────────────────

    void _download(const char* version, unsigned long now) {
#if ARDUINO_SIGNING
        _tagged.tag(version);   // the signature must name the version asked for
#endif
        if (!_tlsReady()) {
            _recoverHeap();
            _downloadFailed(now);
            return;
        }
        _path = (char*)malloc(OTA_PATH_CAP);
        if (!_path) {
            _note(Report::Failed, PSTR("Not enough memory"));
            _log.log(PSTR("OTA: not enough memory"));
            _recoverHeap();
            _downloadFailed(now);
            return;
        }
        // Fixed host and path; only the tag (≤15) and the asset vary, and they fit
        strlcpy(_host, "github.com", sizeof(_host));
        snprintf_P(_path, OTA_PATH_CAP,
                   PSTR("/allanbarcelos/esp8266-dns/releases/download/%s/%s"),
                   version, assetName(signingEnabled()));
        _log.log(PSTR("Firmware URL: https://%s%s"), _host, _path);
        _hop = 0;
        _downloadConnect(now);
    }

    void _downloadConnect(unsigned long now) {
        if (!_prepareTls(_host, _hop > 0)) {
            _note(Report::Failed, PSTR("TLS client unavailable"));
            _log.log(PSTR("OTA: TLS unavailable"));
            _downloadFailed(now);
            return;
        }
        if (!_client.connect(_host, 443)) {
            _note(Report::Failed, PSTR("Firmware download failed"));
            _log.log(PSTR("Could not start OTA"));
            _downloadFailed(now);
            return;
        }
        _sendGet(_host, _path);
        _hdrP.begin(_path, OTA_PATH_CAP); // Location reuses the buffer
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
            _note(Report::Failed, PSTR("Incomplete OTA response"));
            _log.log(PSTR("OTA: incomplete headers"));
            _downloadFailed(now);
            return;
        }
        int code = _hdrP.code;
        if (_isRedirect(code)) {
            _log.log(PSTR("OTA redirect %d"), code);
            _client.stop();
            if (_path[0] == '\0') {
                _note(Report::Failed, PSTR("OTA redirect without Location"));
                _log.log(PSTR("OTA: redirect without Location"));
                _downloadFailed(now);
                return;
            }
            if (_path[0] != '/' &&
                !_splitUrl(_path, _host, sizeof(_host), _path, OTA_PATH_CAP)) {
                _note(Report::Failed, PSTR("Invalid OTA redirect"));
                _log.log(PSTR("OTA: invalid redirect"));
                _downloadFailed(now);
                return;
            }
            if (++_hop >= MAX_REDIRECTS) {
                _note(Report::Failed, PSTR("OTA exceeded the redirects"));
                _log.log(PSTR("OTA: too many redirects"));
                _downloadFailed(now);
                return;
            }
            _downloadConnect(now);
            return;
        }
        if (code != 200) {
            _note(Report::Failed, PSTR("Download HTTP %d"), code);
            _log.log(PSTR("OTA HTTP error: %d"), code);
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
            _note(Report::Failed, PSTR("Invalid firmware size"));
            _log.log(PSTR("Invalid firmware size"));
            _downloadFailed(now);
            return;
        } else {
            flashSize = (size_t)_contentLength;
        }

        if (flashSize == 0 || !Update.begin(flashSize)) {
            _note(Report::Failed, PSTR("No room for the firmware"));
            _log.log(PSTR("No room for OTA: %s"), Update.getErrorString().c_str());
            _downloadFailed(now);
            return;
        }

        _written      = 0;
        _lastPct      = 0;
        _lastProgress = now;
        _inProgress   = true;
        _wifi.setOTAInProgress(true);
        if (_unknownLen) _log.log(PSTR("OTA started: unknown size"));
        else _log.log(PSTR("OTA started: %d bytes"), _contentLength);
    }

    void _streamChunk(unsigned long now) {
        if (!_wifi.isConnected()) {
            _abort("Wi-Fi dropped during OTA", now);
            return;
        }
        if (now - _lastProgress > STALL_TIMEOUT) {
            _abort("OTA without data, aborting", now);
            return;
        }

        size_t avail = (size_t)_client.available();
        if (avail == 0) {
            if (!_unknownLen) return;
            if (_client.connected()) return;
            if (_written == 0) {
                _abort("Empty OTA", now);
                return;
            }
            _complete(true);
            return;
        }

        uint8_t buf[512];
        size_t len = _client.readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));

        if (Update.write(buf, len) != len) {
            _abort("OTA write error", now);
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
            _log.log(PSTR("OTA done! Restarting..."));
            _note(Report::UpToDate, PSTR("Firmware updated"));
            _wifi.scheduleRestart(1000);
        } else {
            _note(Report::Failed, PSTR("Image rejected: %s"), Update.getErrorString().c_str());
            _log.log(PSTR("OTA error: %s"), Update.getErrorString().c_str());
            strlcpy(_rejected, _available, sizeof(_rejected));
            _saveRejected();
            bool restore = _slim;
            _tries = 0;
            _retryDelay = 0;
            _clearPending();
            if (restore) {
                _log.log(PSTR("OTA: recovery over, restoring the panel"));
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
            if (install) { // an install is only requested with _available set
                _persistAndReboot();
                return;
            }
            _note(Report::Failed, PSTR("OTA failed, will try again later"));
            _log.log(PSTR("OTA: out of retries"));
            return;
        }
        unsigned long delayMs = _retryDelay == 0 ? RETRY_MS : _retryDelay * 2;
        if (delayMs > RETRY_MAX_MS) delayMs = RETRY_MAX_MS;
        _retryDelay = delayMs;
        _nextRetry = now + delayMs;
        if (install) _wantInstall = true;
        else _wantCheck = true;
        _note(Report::Failed, PSTR("Retrying in %u s"), (unsigned)(delayMs / 1000));
        _log.log(PSTR("OTA: retrying in %u s"), (unsigned)(delayMs / 1000));
    }

    void _persistAndReboot() {
        uint8_t boots = (uint8_t)(_pendingBoots + 1);
        if (boots > MAX_BOOTS) {
            _clearPending();
            _wantInstall = false;
            _wantCheck = false;
            _note(Report::Failed, PSTR("OTA gave up after recovery"));
            _log.log(PSTR("OTA: gave up after recovery"));
            _wifi.scheduleRestart(1500);
            return;
        }
        if (!_savePending(_available, boots)) _log.log(PSTR("OTA: could not save the recovery request"));
        else _log.log(PSTR("OTA: recovery on the next boot"));
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
            _log.log(PSTR("OTA: could not save the rejected version"));
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
        // "<tag> <boots>" read by hand: sscanf would pull scanf into the link (~2.9 KB)
        const char* p = buf;
        while (*p == ' ' || *p == '\n') p++;
        char tag[16];
        size_t t = 0;
        while (t < sizeof(tag) - 1 && p[t] && p[t] != ' ' && p[t] != '\n') {
            tag[t] = p[t];
            t++;
        }
        tag[t] = '\0';
        unsigned boots = (unsigned)strtoul(p + t, nullptr, 10);
        bool isCheck = strcmp(tag, "check") == 0;
        if (t == 0 || (!isCheck && parseTag(tag) < 0)) {
            LittleFS.remove(PENDING_PATH);
            _log.log(PSTR("OTA: invalid recovery request"));
            return;
        }
        if (boots > MAX_BOOTS) {
            LittleFS.remove(PENDING_PATH);
            _log.log(PSTR("OTA: recovery attempts exhausted"));
            return;
        }
        _pendingBoots = (uint8_t)boots;
        _slim = true;
        // Before trying: if this boot crashes, the next one already counts one
        // more. If it cannot save, recovery goes on with the last request's count.
        (void)_savePending(tag, (uint8_t)(boots + 1));
        if (isCheck) {
            _available[0] = '\0';
            _wantCheck = true;
            _note(Report::Checking, PSTR("Resuming the check"));
            _log.log(PSTR("OTA: resuming the check (boot %u)"), (unsigned)_pendingBoots);
            return;
        }
        strlcpy(_available, tag, sizeof(_available));
        _wantInstall = true;
        _note(Report::Available, PSTR("Resuming %s"), _available);
        _log.log(PSTR("OTA: resuming %s (boot %u)"), _available, (unsigned)_pendingBoots);
    }
};
