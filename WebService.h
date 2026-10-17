// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "HttpServer.h"
#include <LittleFS.h>
#include "Clock.h"
#include "Config.h"
#include "Totp.h"
#include "Password.h"
#include "Logger.h"
#include "Version.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "OTAService.h"
#include "IoTService.h"
#include "NtfyNotifier.h"
#include "MqttService.h"
#include "PowerService.h"
#include "CloudflareSetup.h"
#include "RelayService.h"
#include "NetArbiter.h"
#include "Assets.h"
#include "WebAssets.h"
#include "I18n.h"

// ── PROGMEM ───────────────────────────────────────────────────────────────────
// Header split so a <meta refresh> can go before </head>. The CSS
// (web/panel.css) comes from the device itself, gzipped and cached: in AP
// mode there is no internet to fetch a framework.
//
// Panel text goes out as I18n.h markers (L_...): /i18n.js, also from the
// device and cached, writes the page in the language from Settings or the
// browser's. Logs and the API stay in English.

static const char _HTML_HEAD_A[] PROGMEM =
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>";

static const char _HTML_HEAD_B[] PROGMEM =
    "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"
    "</head><body><header><b>&#9679; ";

// The menu's links and the footer's credit come from /i18n.js (gzipped and
// cached): <nav id='mn'> gets the links, the footer the rest after the version.
static const char _HTML_FOOT_A[] PROGMEM = "</main><footer><span>";

// Attributes for the 6-digit authenticator field. Not a class member:
// C++11 cannot define a non-integral static inside the class.
static const char _CODE_FIELD[] PROGMEM =
    "required inputmode='numeric' pattern='[0-9]{6}' minlength='6' maxlength='6' autocomplete='one-time-code'";

// The panel language goes after the #: the browser does not send it, so
// the cache keeps a single copy of i18n.js
static const char _HTML_FOOT_B[] PROGMEM =
    "</span></footer><script src='/i18n.js?v=" WEB_I18N_TAG "#";

// Panel languages (Settings), in their own names; "" = the browser's
static const char* const _LANG_CODES[] = {"en", "pt", "es", "fr"};
static const char* const _LANG_NAMES[] = {"English", "Português", "Español", "Français"};

// Official time zones, from UTC-12 to UTC+14, in minutes, and a place for
// each (same order, separated by \0). No daylight saving: whoever has it
// picks the zone for the season.
static const int16_t _TZ_MIN[] PROGMEM = {
    -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120,
    -60, 0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420,
    480, 525, 540, 570, 600, 630, 660, 720, 765, 780, 840,
};
static const char _TZ_PLACES[] PROGMEM =
    "Baker Island\0"
    "American Samoa\0"
    "Hawaii\0"
    "Marquesas Islands\0"
    "Alaska\0"
    "Los Angeles\0"
    "Denver\0"
    "Mexico City\0"
    "Acre, New York\0"
    "Manaus, Santiago\0"
    "Newfoundland\0"
    "Brasilia, Buenos Aires\0"
    "Fernando de Noronha\0"
    "Azores\0"
    "London, Lisbon\0"
    "Paris, Luanda\0"
    "Athens, Maputo\0"
    "Moscow\0"
    "Tehran\0"
    "Dubai\0"
    "Kabul\0"
    "Karachi\0"
    "India\0"
    "Nepal\0"
    "Dhaka\0"
    "Myanmar\0"
    "Bangkok\0"
    "Beijing\0"
    "Eucla\0"
    "Tokyo\0"
    "Adelaide\0"
    "Sydney\0"
    "Lord Howe\0"
    "Noumea\0"
    "Auckland\0"
    "Chatham\0"
    "Tonga\0"
    "Kiritimati\0";

// Fixed page text lives in flash (PSTR/PROGMEM) and goes out through
// sendContent_P: a plain literal takes RAM on the ESP8266, and free heap is
// what the OTA TLS needs. A PGM_P parameter takes flash or RAM (the _P reads
// work on both); never pass a PGM_P to String(), strcmp or printf without
// _P: on the device that raises an Exception.
// ─────────────────────────────────────────────────────────────────────────────

class WebService {
public:
    static constexpr size_t        STATE_JSON_LEN    = 384;
    static constexpr size_t        MIN_CMD_TOPIC     = 20;   // "Generate" makes 28
    static constexpr uint8_t       FREE_LOGIN_FAILS  = 5;
    static constexpr unsigned long LOGIN_WAIT_MS     = 2000UL;
    static constexpr unsigned long LOGIN_WAIT_MAX_MS = 300000UL;
    static constexpr unsigned long SESSION_MS     = 12UL * 60UL * 60UL * 1000UL;

    WebService(Config& cfg, Logger& log, ConfigStore& store,
               WiFiService& wifi, OTAService& ota, IoTService& iot,
               NtfyNotifier* ntfy = nullptr, MqttService* mqtt = nullptr,
               PowerService* power = nullptr, CloudflareSetup* cf = nullptr)
        : _server(80), _cfg(cfg), _log(log), _store(store),
          _wifi(wifi), _ota(ota), _iot(iot), _ntfy(ntfy), _mqtt(mqtt), _power(power), _cf(cf) {
        _csrfToken[0] = '\0';
    }

    void begin() {
        _generateCSRF();
        _server.begin(_dispatch, this);
        _log.log(PSTR("Web server listening on port 80"));
    }

    void tick() { _server.handleClient(); }

    // For the remote access relay, which hands requests to the same server
    HttpServer& server() { return _server; }

    // The relay, built after the panel (it uses server()): for its status
    void setRelay(RelayService* relay) { _relay = relay; }

    // Who has the network and the slowest loop, for Status
    void setNet(NetArbiter* net) { _net = net; }

    // Boot token (sign-in and first setup) and the token of the forms of
    // the current session (the boot one while there is none)
    const char* csrfToken() const { return _csrfToken; }
    const char* sessionCsrfToken() const { return _session ? _csrfSid : _csrfToken; }

    // Mask for sensitive tokens: shows only the last 4 characters
    static String maskSecret(const char* s) {
        size_t len = strlen(s);
        if (len == 0) return String();
        return String("****") + String(s + (len > 4 ? len - 4 : 0));
    }

    // Text into buf (DURATION_LEN): "1h 01m 01s"
    static constexpr size_t DURATION_LEN = 48;   // also the "ago" marker; room for 64-bit hosts
    static const char* formatDuration(unsigned long ms, char* buf) {
        unsigned long s = ms / 1000;
        snprintf_P(buf, DURATION_LEN, PSTR("%luh %02lum %02lus"), s / 3600, (s / 60) % 60, s % 60);
        return buf;
    }

    // Binary scale: 1024 B = 1 KB. One decimal place, and a .0 is dropped.
    // 1500 → "1.5 KB"; 1024 → "1 KB"; 1048576 → "1 MB".
    static constexpr size_t BYTES_LEN = 16;   // "4294967295 B"
    static const char* formatBytes(uint32_t n, char* buf) {
        const char* unit = "B";
        uint32_t scale = 1;
        if (n >= 1024UL * 1024UL * 1024UL) {
            unit = "GB";
            scale = 1024UL * 1024UL * 1024UL;
        } else if (n >= 1024UL * 1024UL) {
            unit = "MB";
            scale = 1024UL * 1024UL;
        } else if (n >= 1024UL) {
            unit = "KB";
            scale = 1024UL;
        }
        if (scale == 1) {
            snprintf(buf, BYTES_LEN, "%lu B", (unsigned long)n);
            return buf;
        }
        unsigned long tenths =
            (unsigned long)(((unsigned long long)n * 10ULL + scale / 2) / scale);
        if (tenths % 10 == 0)
            snprintf(buf, BYTES_LEN, "%lu %s", tenths / 10, unit);
        else
            snprintf(buf, BYTES_LEN, "%lu.%lu %s", tenths / 10, tenths % 10, unit);
        return buf;
    }

private:
    HttpServer       _server;
    Config&          _cfg;
    Logger&          _log;
    ConfigStore&     _store;
    WiFiService&     _wifi;
    OTAService&      _ota;
    IoTService&      _iot;
    NtfyNotifier*    _ntfy;
    MqttService*     _mqtt;
    PowerService*    _power;
    CloudflareSetup* _cf;
    RelayService*    _relay = nullptr;
    NetArbiter*      _net = nullptr;
    // Wrong password, code or token: the first mistakes pass; after that each
    // one doubles the wait (2 s up to 5 min), and nothing is tried meanwhile
    // (429). Getting it right resets the count.
    struct Throttle {
        uint8_t       fails = 0;
        unsigned long until = 0;

        bool waiting() const { return fails >= FREE_LOGIN_FAILS && (long)(millis() - until) < 0; }

        // true on the mistake that starts the waits
        bool fail() {
            if (fails < 255) fails++;
            if (fails < FREE_LOGIN_FAILS) return false;
            uint8_t doubling = fails - FREE_LOGIN_FAILS;
            unsigned long wait = LOGIN_WAIT_MS << (doubling < 8 ? doubling : 8);
            until = millis() + (wait < LOGIN_WAIT_MAX_MS ? wait : LOGIN_WAIT_MAX_MS);
            return fails == FREE_LOGIN_FAILS;
        }
    };
    // Three counts, so a device on the network that keeps guessing cannot
    // hold the owner out: the API token has its own, and so has the address
    // of the last sign-in (in RAM; a table per address does not fit, and from
    // outside the home the panel sits behind a tunnel with its own login,
    // docs/REMOTE-ACCESS.md)
    Throttle         _apiTries, _panelTries, _ownerTries;
    uint32_t         _owner = 0;
    uint8_t          _session = 0;   // 0 signed out, 1 authenticator setup, 2 signed in
    char             _sid[33] = {};
    unsigned long    _sidUntil = 0;
    uint32_t         _sidIp = 0;     // the local address the session belongs to (0: through the relay)
    uint8_t          _pending[Totp::KEY_LEN] = {};
    bool             _pendingSet = false;

    char _csrfToken[17]; // 16 hex + null
    char _csrfSid[17] = {};  // the session's form token; empty with no session

    static void _dispatch(void* self) { static_cast<WebService*>(self)->_route(); }

    // Route by method and path, from tables in flash; anything else is a 404.
    // The local API (/api/...) takes a token instead of the panel session.
    struct Route {
        uint8_t method;
        PGM_P   path;
        void (*handler)(WebService&);
    };

    // Embedded files (Assets.h, WebAssets.h): public, with no device data.
    // The type is in RAM: it goes into the header's %s.
    // Each one gzipped, with a hash of its content in the URL (?v=)
    struct Asset {
        PGM_P          path;
        const char*    type;
        const uint8_t* data;
        uint16_t       len;
    };

    static constexpr char P_ROOT[] PROGMEM = "/";
    static constexpr char P_LOG[] PROGMEM = "/log";
    static constexpr char P_STATUS[] PROGMEM = "/status";
    static constexpr char P_CLOUDFLARE[] PROGMEM = "/cloudflare";
    static constexpr char P_NTFY[] PROGMEM = "/ntfy";
    static constexpr char P_MQTT[] PROGMEM = "/mqtt";
    static constexpr char P_PASSWORD[] PROGMEM = "/password";
    static constexpr char P_LOGIN[] PROGMEM = "/login";
    static constexpr char P_MFA[] PROGMEM = "/mfa";
    static constexpr char P_IOT[] PROGMEM = "/iot";
    static constexpr char P_CONFIG[] PROGMEM = "/config";
    static constexpr char P_IOT_NOW[] PROGMEM = "/iot/now";
    static constexpr char P_API_STATE[] PROGMEM = "/api/state";
    static constexpr char P_UPDATE[] PROGMEM = "/update";
    static constexpr char P_SAVE[] PROGMEM = "/save";
    static constexpr char P_CONFIG_SAVE[] PROGMEM = "/config/save";
    static constexpr char P_CLOUDFLARE_SAVE[] PROGMEM = "/cloudflare/save";
    static constexpr char P_CLOUDFLARE_DDNS[] PROGMEM = "/cloudflare/ddns";
    static constexpr char P_CLOUDFLARE_CLEAR[] PROGMEM = "/cloudflare/clear";
    static constexpr char P_NTFY_SAVE[] PROGMEM = "/ntfy/save";
    static constexpr char P_MQTT_SAVE[] PROGMEM = "/mqtt/save";
    static constexpr char P_REMOTE_SAVE[] PROGMEM = "/remote/save";
    static constexpr char P_REMOTE_OFF[] PROGMEM = "/remote/off";
    static constexpr char P_PASSWORD_SAVE[] PROGMEM = "/password/save";
    static constexpr char P_LOGOUT[] PROGMEM = "/logout";
    static constexpr char P_MFA_RESET[] PROGMEM = "/mfa/reset";
    static constexpr char P_IOT_SAVE[] PROGMEM = "/iot/save";
    static constexpr char P_IOT_CMD[] PROGMEM = "/iot/cmd";
    static constexpr char P_IOT_TOKEN[] PROGMEM = "/iot/token";
    static constexpr char P_IOT_CLEAR[] PROGMEM = "/iot/clear";
    static constexpr char P_API_CMD[] PROGMEM = "/api/cmd";
    static constexpr char P_UPDATE_AUTO[] PROGMEM = "/update/auto";
    static constexpr char P_UPDATE_CHECK[] PROGMEM = "/update/check";
    static constexpr char P_UPDATE_INSTALL[] PROGMEM = "/update/install";
    static constexpr char P_S_CSS[] PROGMEM = "/s.css";
    static constexpr char P_IOT_JS[] PROGMEM = "/iot.js";
    static constexpr char P_I18N_JS[] PROGMEM = "/i18n.js";
    static constexpr char P_PINOUT_SVG[] PROGMEM = "/pinout.svg";

    // Text used in several places: one copy in flash (PSTR makes one per use)
    static constexpr char S_WARNING[] PROGMEM = "warning";
    static constexpr char S_SUCCESS[] PROGMEM = "success";
    static constexpr char S_DANGER[] PROGMEM = "danger";
    static constexpr char S_DIV_END[] PROGMEM = "</div>";
    static constexpr char S_TAG_END[] PROGMEM = "'>";
    static constexpr char S_SAVE_FAILED[] PROGMEM = L_SAVE_FAILED;
    static constexpr char S_SAVE[] PROGMEM = L_SAVE;
    static constexpr char S_USER[] PROGMEM = L_USER;
    static constexpr char S_STATE[] PROGMEM = L_STATE;
    static constexpr char S_CLOCK[] PROGMEM = L_CLOCK_NOT_SYNC;
    static constexpr char S_SIGN_IN_FAILED[] PROGMEM = L_SIGN_IN_FAILED;
    static constexpr char S_F_USER[] PROGMEM = "user";
    static constexpr char S_F_PASS[] PROGMEM = "pass";
    static constexpr char S_REQ32[] PROGMEM = "required maxlength='32'";
    static constexpr char S_REQ64[] PROGMEM = "required maxlength='64'";
    static constexpr char S_MAX63[] PROGMEM = "maxlength='63'";
    static constexpr char S_CURRENT_PASS_AC[] PROGMEM = "autocomplete='current-password' ";
    static constexpr char S_PASS8[] PROGMEM = "required minlength='8' maxlength='64'";
    static constexpr char S_CURRENT_PASS[] PROGMEM = L_CURRENT_PASS;
    static constexpr char S_CONFIRM_PASS[] PROGMEM = L_CONFIRM_PASS;
    static constexpr char S_INVALID[] PROGMEM = L_INVALID_VALUE;
    static constexpr char S_STATE_NAME[] PROGMEM = "maxlength='15' placeholder='" L_TYPE_DEFAULT "'";
    static constexpr char S_OPTION_END[] PROGMEM = "</option>";
    static constexpr char S_CLOUDFLARE[] PROGMEM = "Cloudflare";
    static constexpr char S_SSID[] PROGMEM = "SSID";
    static constexpr char S_PUBLIC_IP[] PROGMEM = L_PUBLIC_IP;
    static constexpr char S_CONNECTION[] PROGMEM = L_CONNECTION;
    static constexpr char S_PANEL_PASSWORD[] PROGMEM = L_PANEL_PASSWORD;
    static constexpr char S_MFA_REPLACE[] PROGMEM = L_MFA_REPLACE;
    static constexpr char S_VOLTAGE[] PROGMEM = L_VOLTAGE;
    static constexpr char S_RATE[] PROGMEM = L_RATE;
    static constexpr char S_SIGN_IN[] PROGMEM = L_SIGN_IN;
    static constexpr char S_UNKNOWN_ZONE[] PROGMEM = L_UNKNOWN_ZONE;
    static constexpr char S_TYPE[] PROGMEM = L_TYPE;
    static constexpr char S_PIN[] PROGMEM = L_PIN;
    static constexpr char S_F_NAME[] PROGMEM = "name";
    static constexpr char S_F_ZONE[] PROGMEM = "zone";
    static constexpr char S_F_TOKEN[] PROGMEM = "token";
    static constexpr char S_F_HOST[] PROGMEM = "host";
    static constexpr char S_F_PASS1[] PROGMEM = "pass1";
    static constexpr char S_F_PASS2[] PROGMEM = "pass2";
    static constexpr char S_LABEL_END[] PROGMEM = "></label>";
    static constexpr char S_CHARGE[] PROGMEM = L_CHARGE;
    static constexpr char S_TOTAL[] PROGMEM = "Total";
    static constexpr char S_RSSI[] PROGMEM = "RSSI";
    static constexpr char S_DDNS[] PROGMEM = "DDNS";
    static constexpr char S_PERCENT[] PROGMEM = "%u%%";
    static constexpr char S_DBM[] PROGMEM = "%d dBm";

    // The relay Worker's intervals for the ntfy commands, in seconds (relay/worker.mjs POLLS)
    static constexpr uint8_t POLLS[] = {2, 5, 10, 30};

    static const Route ROUTES[];
    static const Asset ASSETS[];
    static const size_t ROUTE_COUNT, ASSET_COUNT;

    void _route() {
        if (!_hostOk()) {
            _server.send(421, "text/plain", "Unknown address; add it under Settings, Other addresses");
            return;
        }
        const char* p = _server.path();
        uint8_t m = _server.method();
        for (size_t i = 0; i < ROUTE_COUNT; i++) {
            Route r;
            memcpy_P(&r, &ROUTES[i], sizeof(r));
            if (r.method == m && !strcmp_P(p, r.path)) return r.handler(*this);
        }
        for (size_t i = 0; i < ASSET_COUNT; i++) {
            Asset f;
            memcpy_P(&f, &ASSETS[i], sizeof(f));
            if (m == HttpServer::GET && !strcmp_P(p, f.path))
                return _asset(f.type, f.data, f.len);
        }
        _server.send(404, "text/plain", "");
    }

    // ── Security ──────────────────────────────────────────────────────────────

    // DNS rebinding: a page on another site that points its own name at the
    // device's address would reach the panel as that site, from inside the
    // home. So only the names the device answers to: an IP address, a name
    // with no dot (the router's DNS), the hostname with any domain after it
    // (garage.local), the Cloudflare host, the relay's address, and the names
    // listed in Settings (a tunnel or a proxy). No Host header at all is not
    // a browser.
    bool _hostOk() const {
        const char* h = _server.host();
        if (*h == '?') return false;
        if (!*h || !strchr(h, '.') || strspn(h, "0123456789.") == strlen(h)) return true;
        char own[sizeof(_cfg.hostname)];
        if (_cfg.hostname[0]) strlcpy(own, _cfg.hostname, sizeof(own));
        else snprintf(own, sizeof(own), "esp-%06x", (unsigned)ESP.getChipId());   // the core's ESP-XXXXXX
        size_t label = strcspn(h, ".");
        if (strlen(own) == label && strncasecmp(h, own, label) == 0) return true;
        return strcasecmp(h, _cfg.cf_host) == 0 || strcasecmp(h, _cfg.relay_host) == 0 ||
               _listed(_cfg.web_hosts, h);
    }

    // h is one of the comma-separated names in list
    static bool _listed(const char* list, const char* h) {
        size_t n = strlen(h);
        for (const char* p = list; *p;) {
            size_t k = strcspn(p, ",");
            if (k == n && strncasecmp(p, h, n) == 0) return true;
            p += k + (p[k] == ',');
        }
        return false;
    }

    // Token from the hardware RNG on every boot and every password change.
    // It guards the forms used before a session exists (sign-in, first
    // setup), so the sign-in page shows it to anyone. Signed-in forms carry
    // the session's own token instead (_csrfSid).
    void _generateCSRF() {
        snprintf(_csrfToken, sizeof(_csrfToken), "%08lx%08lx",
                 (unsigned long)ESP.random(), (unsigned long)ESP.random());
    }

    // Signed-in panel. With no password set, only AP mode (first setup) and the
    // password page (allowSetup) are open. After that the browser must hold
    // the session cookie, and the authenticator must already be enrolled.
    bool _guard(bool allowSetup = false) {
        if (_cfg.webpss.length() == 0) {
            if (allowSetup || _wifi.inConfigMode()) return true;
            _redirect("/password");
            return false;
        }
        // cppcheck-suppress knownConditionTrueFalse ; the counter changes between requests
        if (_loginBlocked()) return false;
        if (!_sessionOk()) {
            _redirect("/login");
            return false;
        }
        if (_session != 2) {
            _redirect("/mfa");
            return false;
        }
        return true;
    }

    // Authenticator enrollment: password checked, code not confirmed yet.
    bool _enrollGuard() {
        if (_cfg.webpss.length() == 0) {
            _redirect("/password");
            return false;
        }
        // cppcheck-suppress knownConditionTrueFalse ; the counter changes between requests
        if (_loginBlocked()) return false;
        if (!_sessionOk() || _session != 1) {
            _redirect(_sessionOk() ? "/" : "/login");
            return false;
        }
        return true;
    }

    // Sign-in redirects. The update pages keep _go (303 after a POST).
    void _redirect(const char* to) {
        _noStore();
        _server.sendHeader("Location", to);
        _server.send(302, "text/plain", "");
    }

    // sid= plus 32 hex digits, as its own cookie. Compared in full.
    bool _cookieMatches() const {
        const char* c = _server.cookie().c_str();
        while (*c) {
            while (*c == ' ' || *c == ';') c++;
            if (!*c) break;
            if (strncmp(c, "sid=", 4) == 0) {
                c += 4;
                uint8_t d = 0;
                for (int i = 0; i < 32; i++) {
                    char g = c[i];
                    if (!g || g == ';') return false;
                    d |= (uint8_t)(g ^ _sid[i]);
                }
                if (c[32] && c[32] != ';') return false;
                return d == 0;
            }
            while (*c && *c != ';') c++;
        }
        return false;
    }

    // At home the panel is plain HTTP: a session started there only counts
    // from the same address, so a cookie seen on the network (by ARP
    // spoofing, say) is worth nothing from another device. Through the relay
    // it is HTTPS, and a phone's address changes: not tied.
    bool _sessionOk() {
        if (_session == 0) return false;
        if ((long)(millis() - _sidUntil) >= 0) {
            _session = 0;
            _sid[0] = '\0';
            return false;
        }
        return _cookieMatches() && (!_sidIp || _server.remoteIP() == _sidIp);
    }

    void _writeCookie(unsigned long maxAge) {
        char buf[128];
        snprintf_P(buf, sizeof(buf),
                   PSTR("sid=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%lu"),
                   _session ? _sid : "", maxAge);
        _server.sendHeader("Set-Cookie", buf);
    }

    // One session. A new sign-in replaces the previous cookie. Not marked
    // Secure: the ESP-01 serves plain HTTP, and a Secure cookie would never
    // be sent. 12 h, and gone on reboot (it lives only in RAM). Every change
    // of level draws a new cookie and a new form token, so one seen before
    // the authenticator code was checked is worth nothing after.
    void _startSession(uint8_t level) {
        snprintf(_sid, sizeof(_sid), "%08lx%08lx%08lx%08lx",
                 (unsigned long)ESP.random(), (unsigned long)ESP.random(),
                 (unsigned long)ESP.random(), (unsigned long)ESP.random());
        snprintf(_csrfSid, sizeof(_csrfSid), "%08lx%08lx",
                 (unsigned long)ESP.random(), (unsigned long)ESP.random());
        _session = level;
        _sidUntil = millis() + SESSION_MS;
        _sidIp = _server.local() ? _server.remoteIP() : 0;
        _writeCookie(SESSION_MS / 1000);
    }

    void _clearSession() {
        _session = 0;
        _sid[0] = '\0';
        _csrfSid[0] = '\0';
        _pendingSet = false;
        _writeCookie(0);
    }

    // Fixed-length compare so a shorter guess is not faster. cap is the
    // longest value the panel will store (32 for the user, 64 for the password).
    static bool _spanEq(const char* a, size_t na, const char* b, size_t nb, size_t cap) {
        uint8_t d = (na > cap || nb > cap || na != nb) ? 1 : 0;
        for (size_t i = 0; i < cap; i++) {
            unsigned char ca = i < na ? (unsigned char)a[i] : 0;
            unsigned char cb = i < nb ? (unsigned char)b[i] : 0;
            d |= (uint8_t)(ca ^ cb);
        }
        return d == 0;
    }

    bool _credentialOk(const String& user, const String& pass) const {
        bool u = _spanEq(user.c_str(), user.length(), _cfg.webusr.c_str(), _cfg.webusr.length(), 32);
        bool p = Password::check(pass, _cfg.webpss);
        return u && p;
    }

    bool _mfaOn() const { return !Totp::blank(_cfg.totp); }

    // Time to check a code against: the device clock once SNTP has set it.
    // Until then (setup access point, no internet, a reboot without RAM
    // clock) the browser's, which i18n.js puts in t when the form is sent.
    // A time the browser makes up does not bring an old code back: every
    // step at or before the last one accepted (kept in flash) is refused, so
    // it only opens a code nobody has seen, which would still have to be
    // guessed under the same wait. now 0: no usable time.
    struct CodeTime {
        time_t now = 0;
        bool   browser = false;
    };

    CodeTime _codeTime() const {
        CodeTime t;
        t.now = _iot.unixTime();
        if (t.now) return t;
        String arg = _server.arg("t");
        const char* s = arg.c_str();
        char* end;
        if (arg.length() > 10 || *s < '0' || *s > '9') return t;
        unsigned long v = strtoul(s, &end, 10);
        if (*end) return t;
        t.browser = true;
        t.now = v > (unsigned long)Clock::MIN_VALID_EPOCH && v < 4000000000UL ? (time_t)v : 0;
        return t;
    }

    // The step a new code must pass. Steps accepted against the browser's
    // clock are kept apart (totpBrowserStep): once the device has its own
    // time, one from a browser clock that ran ahead is ignored, instead of
    // refusing every code until that day comes.
    uint32_t _floor(const CodeTime& t) const {
        uint32_t b = _cfg.totpBrowserStep;
        if (!t.browser && b > (uint32_t)t.now / Totp::STEP + 1) b = 0;
        return b > _cfg.totpStep ? b : _cfg.totpStep;
    }

    void _spend(const CodeTime& t, uint32_t step) {
        if (t.browser) _cfg.totpBrowserStep = step;
        else _cfg.totpStep = step;
    }

    // Code from the form against key; step starts at the last accepted one
    // and gets the step that matched. The caller stores it in totpStep only
    // when the whole check passed. A blank key never matches: its codes are
    // public.
    bool _codeOk(const uint8_t* key, time_t now, uint32_t& step) const {
        if (Totp::blank(key)) return false;
        return Totp::verify(key, now, _server.arg("code").c_str(), &step);
    }

    // A signed-in action that asks again for the current password and a
    // code (new password, new authenticator): the cookie alone, readable on
    // plain HTTP, is not enough. A wrong one is a counted mistake. On
    // success the code is spent in RAM; the caller saves the config.
    bool _confirmed(PGM_P back = P_CONFIG) {
        CodeTime t = _codeTime();
        if (!t.now) {
            _errorPage(S_CLOCK, back);
            return false;
        }
        uint32_t step = _floor(t);
        bool passOk = _credentialOk(_cfg.webusr, _server.arg("pass"));
        bool codeOk = _codeOk(_cfg.totp, t.now, step);
        if (!passOk || !codeOk) {
            _loginFailed(_tries());
            _errorPage(S_SIGN_IN_FAILED, back);
            return false;
        }
        _spend(t, step);
        return true;
    }

    void _ensurePending() {
        if (_pendingSet) return;
        Totp::randomKey(_pending);
        _pendingSet = true;
    }

    // The panel's count for this request: the owner's address has its own
    Throttle& _tries() { return _owner && _server.remoteIP() == _owner ? _ownerTries : _panelTries; }

    bool _blocked(const Throttle& t) {
        // cppcheck-suppress knownConditionTrueFalse ; the counter changes between requests
        if (!t.waiting()) return false;
        _server.send(429, "text/plain", "Too many attempts; please wait");
        return true;
    }

    bool _loginBlocked() { return _blocked(_tries()); }

    void _loginFailed(Throttle& t) {
        if (t.fail()) _log.log(PSTR("Login: repeated failures, growing wait"));
    }

    // Password and code right: this address becomes the owner's
    void _signedIn() {
        _tries().fails = 0;
        uint32_t ip = _server.remoteIP();
        if (ip == _owner) return;
        _owner = ip;
        _ownerTries = Throttle();
    }

    // The session's token for a browser holding the cookie, otherwise the
    // boot one. signIn: the sign-in form, which always carries the boot one.
    const char* _formToken(bool signIn = false) {
        return !signIn && _sessionOk() ? _csrfSid : _csrfToken;
    }

    bool _checkCSRF(bool signIn = false) {
        String got = _server.arg("_csrf");
        const char* want = _formToken(signIn);
        // An empty token never matches. After begin() neither token can be
        // empty, so this is only a guard: both sides run, no short circuit.
        // cppcheck-suppress bitwiseOnBoolean
        if ((*want == '\0') | (!_spanEq(got.c_str(), got.length(), want, strlen(want), sizeof(_csrfToken) - 1))) {
            _server.send(403, "text/plain", "Invalid CSRF token");
            return false;
        }
        return true;
    }

    // ── HTML helpers ──────────────────────────────────────────────────────────

    // Pages and API: nothing cached (Status shows the AP password) and nothing
    // framed by another site (clickjacking)
    void _noStore() {
        _server.sendHeader("Cache-Control", "no-store");
        _server.sendHeader("X-Frame-Options", "DENY");
        _server.sendHeader("X-Content-Type-Options", "nosniff");
    }

    void _pageBegin(int refreshSec = 0, int code = 200) {
        _noStore();
        // Scripts, styles and requests only from the device: even text that
        // slipped past the escaping could not load or send anything elsewhere
        // (inline: the page's own data and handlers, and the swatches' colours;
        // blob: the pinout redrawn in the panel's language)
        _server.sendHeader("Content-Security-Policy", "default-src 'self' 'unsafe-inline';img-src 'self' blob:");
        _server.setContentLength(HttpServer::UNKNOWN_LENGTH);
        _server.send(code, "text/html", "");
        _p(_HTML_HEAD_A);
        _name();
        _p(PSTR("</title>"));
        if (refreshSec > 0) {
            char meta[56];
            snprintf_P(meta, sizeof(meta),
                       PSTR("<meta http-equiv='refresh' content='%d'>"), refreshSec);
            _s(meta);
        }
        _p(_HTML_HEAD_B);
        _name();
        _p(PSTR("</b>"));
        _powerMark();
        // The menu only where its pages open: signed in with the
        // authenticator, or the first setup (no password, access point).
        // Sign-out also while the authenticator is being set up: leaving
        // there drops a replacement and keeps the current key.
        bool session = _sessionOk();
        bool menu = _cfg.webpss.length() ? session && _session == 2 : _wifi.inConfigMode();
        if (menu) _p(PSTR("<nav id='mn'>"));
        else if (session) _p(PSTR("<nav>"));
        if (session) {
            _p(PSTR("<form class='out' action='/logout' method='POST'>"));
            _csrfField();
            _p(PSTR("<button type='submit'>" L_SIGN_OUT "</button></form>"));
        }
        if (menu || session) _p(PSTR("</nav>"));
        _p(PSTR("</header><main>"));
    }

    // Top right corner: on supply, on battery (%) or charging
    void _powerMark() {
        if (!_power) return;
        PowerService::Source s = _power->source();
        _p(s == PowerService::BATTERY && _power->percent() < 20 ? PSTR("<span class='pw lo'")
                                                                 : PSTR("<span class='pw'"));
        if (s == PowerService::MAINS) {
            _p(PSTR(" title='" L_ON_SUPPLY "'>&#9889; " L_POWER "</span>"));
            return;
        }
        if (s == PowerService::UNKNOWN) {
            _p(PSTR(" title='" L_GAUGE_SILENT "'>&#128267; " L_BATTERY " ?</span>"));
            return;
        }
        _p(s == PowerService::CHARGING ? PSTR(" title='" L_BATTERY_CHARGING "'>&#9728; " L_CHARGING " ")
                                       : PSTR(" title='" L_ON_BATTERY "'>&#128267; " L_BATTERY " "));
        _s(String(_power->percent()));
        _p(PSTR("%</span>"));
    }

    // Device name in the title and at the top; without one, the project's
    void _name() {
        if (_cfg.dev_name[0]) _e(_cfg.dev_name);
        else _p(PSTR("ESP8266 DNS"));
    }

    // Footer and the translation; then the page's own scripts, if any
    void _pageFoot() {
        _p(_HTML_FOOT_A);
        _s(firmware_version);
        _p(_HTML_FOOT_B);
        _e(_cfg.lang);
        _p(PSTR("'></script>"));
    }

    void _pageEnd() {
        _pageFoot();
        _p(PSTR("</body></html>"));
    }

    // A page with no known length ends when the connection closes
    void _p(PGM_P s) { _server.sendContent_P(s); }

    // Variable text, escaped for HTML content and attributes, as it goes out
    void _e(const char* s) {
        const char* run = s;
        for (; *s; s++) {
            PGM_P entity = *s == '&'  ? PSTR("&amp;")
                         : *s == '<'  ? PSTR("&lt;")
                         : *s == '>'  ? PSTR("&gt;")
                         : *s == '"'  ? PSTR("&quot;")
                         : *s == '\'' ? PSTR("&#39;") : nullptr;
            if (!entity) continue;
            _server.sendContent(run, (size_t)(s - run));
            _p(entity);
            run = s + 1;
        }
        _server.sendContent(run, (size_t)(s - run));
    }
    void _s(const char* s) { _server.sendContent(s); }
    void _s(const String& s) { _server.sendContent(s); }

    void _title(PGM_P text) {
        _p(PSTR("<h1>"));
        _p(text);
        _p(PSTR("</h1>"));
    }

    void _box(PGM_P title) {
        _p(PSTR("<section class='c'><h2>"));
        _p(title);
        _p(PSTR("</h2>"));
    }

    void _boxEnd() { _p(PSTR("</section>")); }

    // Label + value on one line with a separator (the value is always escaped)
    void _row(PGM_P label, const String& value) { _row(label, value.c_str()); }

    void _row(PGM_P label, const char* value) {
        _p(PSTR("<div class='r'><span>"));
        _p(label);
        _p(PSTR("</span><b>"));
        _e(value);
        _p(PSTR("</b></div>"));
    }

    // An address, "-" while offline
    void _ipRow(PGM_P label, bool con, IPAddress ip) { _row(label, con ? ip.toString().c_str() : "-"); }

    // A row whose value is formatted (fmt in flash)
    void _rowf(PGM_P label, PGM_P fmt, ...) {
        char b[40];
        va_list args;
        va_start(args, fmt);
        vsnprintf_P(b, sizeof(b), fmt, args);
        va_end(args);
        _row(label, b);
    }

    void _csrfField(bool signIn = false) {
        _p(PSTR("<input type='hidden' name='_csrf' value='"));
        _s(_formToken(signIn));
        _p(S_TAG_END);
    }

    // A POST form with its CSRF field; attrs (data-q, id) go in the tag
    void _form(PGM_P action, PGM_P attrs = nullptr, bool signIn = false) {
        _p(PSTR("<form action='"));
        _p(action);
        _p(PSTR("' method='POST'"));
        if (attrs) _p(attrs);
        _p(PSTR(">"));
        _csrfField(signIn);
    }

    // Authenticator code, plus the browser's clock (t, filled in by i18n.js
    // when the form is sent; see _codeTime)
    void _codeInput() {
        _input(PSTR(L_AUTH_CODE), PSTR("code"), "", _CODE_FIELD);
        _p(PSTR("<input type='hidden' name='t'>"));
    }

    void _alertOpen(PGM_P cls) {
        _p(PSTR("<div class='a "));
        _p(cls);
        _p(S_TAG_END);
    }

    // Variable text: always escaped
    void _alert(PGM_P cls, const String& msg) {
        _alertOpen(cls);
        _e(msg.c_str());
        _p(S_DIV_END);
    }

    // Fixed firmware text (PSTR, with no special HTML characters)
    void _alertP(PGM_P cls, PGM_P msg) {
        _alertOpen(cls);
        _p(msg);
        _p(S_DIV_END);
    }

    // The <label> wraps the field: implicit association for screen readers
    void _labelOpen(PGM_P label) {
        _p(PSTR("<label>"));
        _p(label);
    }

    void _input(PGM_P label, PGM_P name, const char* value, PGM_P extra) {
        _labelOpen(label);
        _p(PSTR("<input name='"));
        _p(name);
        _p(PSTR("' value='"));
        _e(value);
        _p(PSTR("' "));
        _p(extra);
        _p(S_LABEL_END);
    }

    void _passwordInput(PGM_P label, PGM_P name, PGM_P extra, PGM_P complete = nullptr) {
        _labelOpen(label);
        _p(PSTR("<input type='password' "));
        _p(complete ? complete : PSTR("autocomplete='new-password' "));
        _p(PSTR("name='"));
        _p(name);
        _p(PSTR("' "));
        _p(extra);
        _p(S_LABEL_END);
    }

    // Checkbox (on/off), checked or not
    void _switch(PGM_P id, PGM_P label, bool on) {
        _p(PSTR("<label class='sw'><input type='checkbox' role='switch' id='"));
        _p(id);
        _p(PSTR("' name='"));
        _p(id);
        _p(on ? PSTR("' value='1' checked>") : PSTR("' value='1'>"));
        _p(label);
        _p(PSTR("</label>"));
    }

    void _button(PGM_P text) {
        _p(PSTR("<button>"));
        _p(text);
        _p(PSTR("</button></form>"));
    }

    void _backButton(PGM_P backHref) {
        _p(PSTR("<a class='b b2' href='"));
        _p(backHref);
        _p(PSTR("'>" L_BACK "</a>"));
    }

    // Confirmation page with an automatic redirect
    void _savedPage(PGM_P msg, PGM_P backHref) {
        _pageBegin();
        _alertP(S_SUCCESS, msg);
        _p(PSTR("<meta http-equiv='refresh' content='2;url="));
        _p(backHref);
        _p(S_TAG_END);
        _backButton(backHref);
        _pageEnd();
    }

    void _errorPage(PGM_P msg, PGM_P backHref) {
        _pageBegin(0, 400);
        _alertP(S_DANGER, msg);
        _backButton(backHref);
        _pageEnd();
    }

    // Copies the argument into dst if it fits; otherwise answers 400
    bool _argInto(const char* name, char* dst, size_t size, const char* backHref) {
        String v = _server.arg(name);
        v.trim();
        if (v.length() >= size) {
            _errorPage(PSTR(L_VALUE_TOO_LONG), backHref);
            return false;
        }
        strlcpy(dst, v.c_str(), size);
        return true;
    }

    // Checks the panel user/password sent in the user/pass1/pass2 fields
    bool _readPanelCredentials(const char* backHref) {
        String user  = _server.arg("user");
        String pass1 = _server.arg("pass1");
        user.trim();
        if (user.length() == 0 || user.length() > 32) {
            _errorPage(PSTR(L_INVALID_USER), backHref);
            return false;
        }
        if (pass1 != _server.arg("pass2")) {
            _errorPage(PSTR(L_PASS_MISMATCH), backHref);
            return false;
        }
        if (pass1.length() < MIN_PANEL_PASSWORD_LEN || pass1.length() > 64) {
            _errorPage(PSTR(L_PASS_LENGTH), backHref);
            return false;
        }
        _cfg.webusr = user;
        _cfg.webpss = Password::hash(pass1);
        return true;
    }

    void _otaProgressBar() {
        if (!_ota.inProgress()) return;
        char b[96], done[BYTES_LEN], total[BYTES_LEN];
        int pct = _ota.progressPercent();
        snprintf_P(b, sizeof(b), PSTR("<b>%d%%</b><progress max='100' value='%d'></progress><span class='m'>%s / %s"),
                   pct, pct, formatBytes((uint32_t)_ota.bytesWritten(), done),
                   formatBytes((uint32_t)_ota.contentLength(), total));
        _p(PSTR("<div class='a' role='status'>" L_OTA_PROGRESS " "));
        _s(b);
        _p(PSTR("</span></div>"));
    }

    // ── Handlers ─────────────────────────────────────────────────────────────

    void _handleRoot() {
        if (!_guard()) return;
        _pageBegin();

        if (_wifi.inConfigMode()) {
            _alertP(S_WARNING, PSTR(L_SETUP_MODE));
            _form(P_SAVE);
            _input(S_SSID, PSTR("ssid"), _cfg.ssid.c_str(), S_REQ32);
            _passwordInput(PSTR(L_WIFI_PASSWORD), S_F_PASS, S_MAX63);
            if (_cfg.webpss.length() == 0) {
                _p(PSTR("<h2>" L_PANEL_ACCESS "</h2>"));
                _input(S_USER, S_F_USER, _cfg.webusr.length() ? _cfg.webusr.c_str() : "admin",
                       S_REQ32);
                _passwordInput(PSTR(L_PANEL_PASS_MIN), S_F_PASS1, S_PASS8);
                _passwordInput(S_CONFIRM_PASS, S_F_PASS2, S_PASS8);
            }
            _button(PSTR(L_CONNECT));
        } else {
            _otaProgressBar();

            _box(PSTR(L_NETWORK));
            _row(S_SSID,       WiFi.SSID());
            _row(PSTR(L_LOCAL_IP),   WiFi.localIP().toString());
            _row(S_PUBLIC_IP, _cfg.publicIP[0] ? _cfg.publicIP : "-");
            _rowf(S_RSSI,      S_DBM, (int)WiFi.RSSI());
            _boxEnd();

            // The parts in use: sensor and actuator, then Cloudflare once it has a token
            if (_iot.hasSensor() || _iot.hasActuator())
                _p(PSTR("<section class='c'><h2>IoT</h2><div id='io'></div></section>"));
            if (_cfg.cf_token[0]) {
                char b[DURATION_LEN];
                _box(S_CLOUDFLARE);
                _row(PSTR(L_REMOTE), _cfg.relay_host[0] ? _cfg.relay_host : "-");
                if (_cfg.relay_host[0] && _relay) _row(S_CONNECTION, _relay->status());
                _row(S_DDNS, _cfg.cf_host[0] ? _cfg.cf_host : "-");
                _row(PSTR(L_LAST_UPDATE), _ago(_cfg.lastDnsUpdate != 0, _cfg.lastDnsUpdate, b));
                _boxEnd();
            }
        }

        if (!_wifi.inConfigMode() && (_iot.hasSensor() || _iot.hasActuator())) {
            _pageFoot();
            _iotScript();
        } else {
            _pageEnd();
        }
    }

    // Device settings: name, hostname, time zone and battery gauge
    void _handleConfig() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR(L_SETTINGS));
        _box(PSTR(L_GENERAL));
        _form(P_CONFIG_SAVE);
        _input(PSTR(L_NAME " <span class='m'>" L_NAME_HINT "</span>"), S_F_NAME,
               _cfg.dev_name, PSTR("maxlength='31' placeholder='ESP8266 DNS'"));
        _input(PSTR("Hostname <span class='m'>" L_HOSTNAME_HINT "</span>"),
               PSTR("hostname"), _cfg.hostname,
               PSTR("maxlength='32' pattern='[A-Za-z0-9]([A-Za-z0-9\\-]*[A-Za-z0-9])?'"));
        _input(PSTR(L_OTHER_NAMES " <span class='m'>" L_OTHER_NAMES_HINT "</span>"), PSTR("hosts"),
               _cfg.web_hosts, PSTR("maxlength='63' pattern='[A-Za-z0-9.,\\-]*' placeholder='esp.example.com'"));
        _tzSelect();
        _pinSelect(PSTR(L_GAUGE " <span class='m'>" L_GAUGE_HINT "</span>"), PSTR("bat_sda"), _cfg.bat_sda,
                   PSTR(L_NONE));
        _selectOpen(PSTR(L_LANGUAGE), PSTR("lang"));
        _option("", PSTR(L_BROWSER_LANG), !_cfg.lang[0]);
        for (size_t i = 0; i < sizeof(_LANG_CODES) / sizeof(_LANG_CODES[0]); i++)
            _option(_LANG_CODES[i], _LANG_NAMES[i], strcmp(_cfg.lang, _LANG_CODES[i]) == 0);
        _selectEnd();
        _button(S_SAVE);
        _boxEnd();

        // The Cloudflare token: the device finds the domains it reaches with
        // it; the Cloudflare page then sets up DDNS and the remote access
        _box(S_CLOUDFLARE);
        if (_cfg.cf_token[0])
            _alert(S_SUCCESS, L_TOKEN_SET " " + maskSecret(_cfg.cf_token));
        else
            _alertP(S_WARNING, PSTR(L_TOKEN_NOT_SET));
        _form(P_CLOUDFLARE_SAVE);
        // An empty field keeps the current token and checks it again
        _passwordInput(PSTR(L_API_TOKEN " <span class='m'>" L_KEEP_BLANK "</span>"),
                       S_F_TOKEN, PSTR("placeholder='" L_NEW_TOKEN "' maxlength='79' pattern='[A-Za-z0-9_\\-]*'"));
        _p(PSTR("<p class='m'>" L_CF_TOKEN_HELP "</p>"));
        _button(PSTR(L_CF_CHECK));
        if (_cfg.cf_token[0]) {
            // i18n.js asks for confirmation (data-q) before it goes
            _form(P_CLOUDFLARE_CLEAR, PSTR(" data-q='" L_CF_CLEAR_ASK "'"));
            _p(PSTR("<button class='bx'>" L_CF_CLEAR "</button></form>"));
        }
        _boxEnd();

        _box(S_PANEL_PASSWORD);
        _passwordForm();
        _boxEnd();
        if (_cfg.webpss.length()) {
            _box(S_MFA_REPLACE);
            _mfaResetForm();
            _boxEnd();
        }
        _pageEnd();
    }

    void _handleSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String ssid = _server.arg("ssid");
        String pass = _server.arg("pass");
        if (ssid.length() == 0 || ssid.length() > 32) {
            _errorPage(PSTR(L_INVALID_SSID), P_ROOT);
            return;
        }
        if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
            _errorPage(PSTR(L_WIFI_PASS_LENGTH), P_ROOT);
            return;
        }
        if (_cfg.webpss.length() == 0 && !_readPanelCredentials("/")) return;

        _cfg.ssid = ssid;
        _cfg.pass = pass;
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_ROOT);
            return;
        }
        _savedPage(PSTR(L_WIFI_SET), P_ROOT);
        _wifi.scheduleRestart(2000);
    }

    // Name: up to 31 bytes, with no quotes, backslash or control characters
    // (it goes unescaped into the discovery JSON). Hostname: letters, digits
    // and hyphen, with no hyphen at either end (RFC 1123). Language: one of
    // _LANG_CODES, or blank for the browser's.
    void _handleConfigSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        String name = _server.arg("name");
        String host = _server.arg("hostname");
        String hosts = _server.arg("hosts");
        int16_t tz = 0;
        String tzArg = _server.arg("tz");
        if (tzArg.length() && (!IoTService::parseI16(tzArg, -720, 840, tz) || !validTz(tz))) {
            _errorPage(PSTR(L_INVALID_TZ), P_CONFIG);
            return;
        }
        String lang = _server.arg("lang");
        bool known = lang.length() == 0;
        for (size_t i = 0; !known && i < sizeof(_LANG_CODES) / sizeof(_LANG_CODES[0]); i++)
            known = lang == _LANG_CODES[i];
        if (!known) {
            _errorPage(PSTR(L_INVALID_LANG), P_CONFIG);
            return;
        }
        int8_t bat = -1;
        String batArg = _server.arg("bat_sda");
        if (batArg.length() && (!IoTService::parsePin(batArg, bat) || !PowerService::pinsFree(bat, _cfg.iot))) {
            _errorPage(PSTR(L_GAUGE_PINS_TAKEN), P_CONFIG);
            return;
        }
        name.trim();
        host.trim();
        bool ok = name.length() < sizeof(_cfg.dev_name);
        for (size_t i = 0; ok && i < name.length(); i++) {
            unsigned char c = (unsigned char)name.c_str()[i];
            ok = c >= 0x20 && c != '"' && c != '\\' && c != 0x7F;
        }
        if (!ok) {
            _errorPage(PSTR(L_INVALID_NAME), P_CONFIG);
            return;
        }
        size_t n = host.length();
        ok = n < sizeof(_cfg.hostname) && (n == 0 || (host.c_str()[0] != '-' && host.c_str()[n - 1] != '-'));
        for (size_t i = 0; ok && i < n; i++) {
            char c = host.c_str()[i];
            ok = isalnum((unsigned char)c) || c == '-';
        }
        if (!ok) {
            _errorPage(PSTR(L_INVALID_HOSTNAME), P_CONFIG);
            return;
        }
        hosts.trim();
        ok = hosts.length() < sizeof(_cfg.web_hosts);
        for (size_t i = 0; ok && i < hosts.length(); i++) {
            char c = hosts.c_str()[i];
            ok = isalnum((unsigned char)c) || c == '.' || c == ',' || c == '-';
        }
        if (!ok) {
            _errorPage(PSTR(L_INVALID_NAMES), P_CONFIG);
            return;
        }
        strlcpy(_cfg.dev_name, name.c_str(), sizeof(_cfg.dev_name));
        strlcpy(_cfg.web_hosts, hosts.c_str(), sizeof(_cfg.web_hosts));
        strlcpy(_cfg.hostname, host.c_str(), sizeof(_cfg.hostname));
        _cfg.tz = tz;
        _cfg.bat_sda = bat;
        strlcpy(_cfg.lang, lang.c_str(), sizeof(_cfg.lang));
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_CONFIG);
            return;
        }
        if (_cfg.hostname[0]) WiFi.hostname(_cfg.hostname);
        if (_mqtt) _mqtt->reconnect();   // discovery carries the new name
        if (_power) _power->begin();
        _savedPage(PSTR(L_SETTINGS_SAVED), P_CONFIG);
    }

    void _handleLog() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR(L_SYSTEM_LOG));
        _p(PSTR("<pre>"));
        uint8_t cnt   = _log.count();
        uint8_t start = _log.startIndex();
        for (uint8_t i = 0; i < cnt; i++) {
            _e(_log.entry(start + i));
            _p(PSTR("\n"));
        }
        _p(PSTR("</pre>"));
        _pageEnd();
    }

    void _handleStatus() {
        if (!_guard()) return;
        _pageBegin(30);
        _title(PSTR(L_STATUS));

        _otaProgressBar();
        char b[DURATION_LEN];

        _box(PSTR(L_SYSTEM));
        _row(PSTR(L_UPTIME),  formatDuration(millis(), b));
        _row(PSTR(L_LOCAL_TIME), _localTime(b));
        _row(PSTR(L_RESET),   ESP.getResetReason());
        _row(PSTR(L_CAUSE),   ESP.getResetInfo());
        _rowf(PSTR(L_CHIP_ID), PSTR("%x"), (unsigned)ESP.getChipId());
        _row(PSTR(L_AP_PASSWORD " " AP_SSID), _wifi.apPassword());
        _rowf(PSTR("CPU"),    PSTR("%u MHz"), (unsigned)ESP.getCpuFreqMHz());
        _row(PSTR("SDK"),     ESP.getSdkVersion());
        _boxEnd();

        _box(PSTR(L_MEMORY));
        _row(PSTR(L_FREE_HEAP),   formatBytes(ESP.getFreeHeap(), b));
        _row(PSTR(L_LARGEST_BLOCK),  formatBytes(ESP.getMaxFreeBlockSize(), b));
        _rowf(PSTR(L_FRAGMENTATION), S_PERCENT, (unsigned)ESP.getHeapFragmentation());
        if (_net) {
            char q[NetArbiter::QUEUE_LEN];
            _row(PSTR(L_NET_TURN), _net->queue(q));
            _rowf(PSTR(L_SLOWEST_LOOP), PSTR("%lu ms (%s)"), _net->worst(), NetArbiter::name(_net->worstWho()));
            _rowf(PSTR(L_SLOW_LOOPS), PSTR("%u"), (unsigned)_net->slow());
        }
        // The loop's stack is 4 KB: past it, the core cannot yield any more
        // and the software watchdog resets the device
        _row(PSTR(L_STACK_FREE), formatBytes(ESP.getFreeContStack(), b));
        _boxEnd();

        _box(PSTR("Flash"));
        _row(S_TOTAL,      formatBytes(ESP.getFlashChipSize(), b));
        _row(PSTR("Sketch"),     formatBytes(ESP.getSketchSize(), b));
        _row(PSTR(L_OTA_RESERVE), formatBytes(FlashLayout::OTA_RESERVE, b));
        _row(PSTR(L_FREE),      formatBytes(ESP.getFreeSketchSpace(), b));
        _boxEnd();

        _box(PSTR("LittleFS"));
        FSInfo fs;
        LittleFS.info(fs);
        _row(S_TOTAL, formatBytes((uint32_t)fs.totalBytes, b));
        _row(PSTR(L_USED), formatBytes((uint32_t)fs.usedBytes, b));
        Dir dir = LittleFS.openDir("/");
        int fc = 0;
        while (dir.next()) fc++;
        _rowf(PSTR(L_FILES), PSTR("%d"), fc);
        _boxEnd();

        _box(PSTR("Wi-Fi"));
        bool con = WiFi.isConnected();
        _row(S_SSID,    con ? WiFi.SSID().c_str()         : "-");
        _ipRow(PSTR("IP"),      con, WiFi.localIP());
        _ipRow(PSTR(L_GATEWAY), con, WiFi.gatewayIP());
        _ipRow(PSTR(L_NETMASK), con, WiFi.subnetMask());
        _ipRow(PSTR("DNS"),     con, WiFi.dnsIP());
        _row(PSTR("MAC"),     WiFi.macAddress());
        _rowf(S_RSSI,   S_DBM, (int)WiFi.RSSI());
        _boxEnd();

        if (_power && _power->configured()) {
            _box(PSTR(L_BATTERY));
            bool ok = _power->source() != PowerService::UNKNOWN;
            _row(S_STATE, PowerService::name(_power->source()));
            unsigned mv = _power->millivolts();
            int r = _power->rateTenths();
            if (ok) {
                _rowf(S_CHARGE,  S_PERCENT, (unsigned)_power->percent());
                _rowf(S_VOLTAGE, PSTR("%u.%02u V"), mv / 1000u, (mv % 1000u) / 10u);
                _rowf(S_RATE,    PSTR("%s%d.%d %%/h"), r < 0 ? "-" : "", abs(r) / 10, abs(r) % 10);
            } else {
                _row(S_CHARGE, "-");
                _row(S_VOLTAGE, "-");
                _row(S_RATE, "-");
            }
            _boxEnd();
        }

        // Checked every 5 min (PublicIPResolver); reverse name from the network's DNS
        _box(S_PUBLIC_IP);
        bool ip = _cfg.publicIP[0];
        _row(PSTR("IP"),      ip ? _cfg.publicIP : "-");
        _row(PSTR(L_REVERSE), _cfg.publicHost[0] ? _cfg.publicHost : "-");
        _row(PSTR(L_CHANGED),   _ago(ip, _cfg.ipChanged, b));
        _row(PSTR(L_LAST_CHECK), _ago(ip, _cfg.ipChecked, b));
        _boxEnd();

        _pageEnd();
    }

    // Everything the token sets up: the remote access Worker and the DDNS
    // record. The token itself is on Settings. The page refreshes while the
    // device works with Cloudflare.
    void _handleCloudflare() {
        if (!_guard()) return;
        uint8_t st = _cf ? _cf->state() : CloudflareSetup::IDLE;
        _pageBegin(st == CloudflareSetup::WORKING ? 3 : 0);
        _title(S_CLOUDFLARE);
        if (!_cf || !_cfg.cf_token[0]) {
            _alertP(S_WARNING, PSTR(L_CF_NEEDS_TOKEN));
            _p(PSTR("<a class='b' href='/config'>" L_SETTINGS "</a>"));
            _pageEnd();
            return;
        }
        if (st == CloudflareSetup::WORKING) {
            _alertP(S_WARNING, PSTR(L_CF_WORKING));
        } else if (st == CloudflareSetup::FAILED) {
            _alert(S_DANGER, String(L_CF_FAILED ": ") + _cf->message());
        } else if (st == CloudflareSetup::DONE && _cf->last() != CloudflareSetup::DEPLOY) {
            char b[24 + sizeof(_cfg.cf_host)];
            if (_cf->last() == CloudflareSetup::SCAN) snprintf_P(b, sizeof(b), PSTR(L_CF_ZONES " %u"), (unsigned)_cf->zones());
            else snprintf_P(b, sizeof(b), PSTR(L_CF_RECORD_READY " %s"), _cfg.cf_host);
            _alert(S_SUCCESS, b);
        }

        // The relay: the device sets up its Worker with the token and keeps a
        // connection to it (RelayService)
        _box(PSTR(L_WORKER));
        _p(PSTR("<p class='m'>" L_REMOTE_HELP "</p>"));
        if (_cfg.relay_host[0]) {
            _row(S_STATE, _relay ? _relay->status() : "-");
            _p(PSTR("<p><a href='https://"));
            _e(_cfg.relay_host);
            _p(PSTR("'>https://"));
            _e(_cfg.relay_host);
            _p(PSTR("</a></p>"));
        }
        _form(P_REMOTE_SAVE);
        _selectOpen(PSTR(L_ADDRESS), S_F_ZONE);
        CloudflareSetup::Zone z;
        File f = LittleFS.open(CloudflareSetup::ZONES_PATH, "r");
        while (f && CloudflareSetup::next(f, z)) _zoneOption(z.id, z.name, strcmp(z.id, _cfg.relay_zone) == 0);
        if (f) f.close();
        _option("", PSTR("workers.dev"), !_cfg.relay_zone[0]);
        _selectEnd();
        _input(PSTR(L_REMOTE_NAME), S_F_NAME, _cfg.relay_name,
               PSTR("required maxlength='32' placeholder='esp-pump' pattern='[a-z0-9\\-]+'"));
        _input(PSTR(L_WD_NAME), PSTR("wd"), "", PSTR("maxlength='63' pattern='[a-z0-9\\-]*'"));
        _button(PSTR(L_REMOTE_ON));
        if (_cfg.relay_host[0]) {
            _form(P_REMOTE_OFF);
            _p(PSTR("<button class='b2'>" L_REMOTE_OFF "</button></form>"));
        }
        _boxEnd();

        _box(S_DDNS);
        _form(P_CLOUDFLARE_DDNS);
        _selectOpen(PSTR(L_DOMAIN), S_F_ZONE);
        _option("", PSTR(L_OFF_CAP), !_cfg.cf_zone[0]);
        bool listed = false;
        f = LittleFS.open(CloudflareSetup::ZONES_PATH, "r");
        while (f && CloudflareSetup::next(f, z)) {
            bool on = strcmp(z.id, _cfg.cf_zone) == 0;
            listed = listed || on;
            _zoneOption(z.id, z.name, on);
        }
        if (f) f.close();
        if (_cfg.cf_zone[0] && !listed) _zoneOption(_cfg.cf_zone, _cfg.cf_zone, true);
        _selectEnd();
        _input(PSTR(L_RECORD_NAME), S_F_NAME, _cfg.cf_host,
               PSTR("maxlength='63' placeholder='home.example.com' pattern='[A-Za-z0-9.\\-]*'"));
        _p(PSTR("<p class='m'>" L_RECORD_HELP "</p>"));
        _button(S_SAVE);
        _boxEnd();
        _pageEnd();
    }

    void _zoneOption(const char* id, const char* name, bool on) {
        _optionOpen(id, on);
        _e(name);
        _p(S_OPTION_END);
    }

    void _handleCloudflareSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        char token[sizeof(_cfg.cf_token)];
        if (!_argInto("token", token, sizeof(token), P_CONFIG)) return;
        // It goes into a request header: a line break would add another
        if (!_tokenText(token)) {
            _errorPage(PSTR(L_INVALID_CF_TOKEN), P_CONFIG);
            return;
        }
        if (token[0]) {
            strlcpy(_cfg.cf_token, token, sizeof(_cfg.cf_token));
            if (!_store.save()) {
                _errorPage(S_SAVE_FAILED, P_CONFIG);
                return;
            }
        }
        if (_cf && _cfg.cf_token[0]) _cf->scan();
        _go(_cfg.cf_token[0] ? "/cloudflare" : "/config");
    }

    // The DDNS record: a domain from the list and a name in it ("home" or
    // "home.example.com"); the name goes into the API request, so only the
    // characters a host name has. No domain turns DDNS off.
    void _handleCloudflareDdns() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        String zone = _server.arg("zone");
        String in = _server.arg("name");
        in.trim();
        if (!zone.length()) {
            _cfg.cf_zone[0] = '\0';
            _cfg.cf_record[0] = '\0';
            if (!_store.save()) {
                _errorPage(S_SAVE_FAILED, P_CLOUDFLARE);
                return;
            }
            _savedPage(PSTR(L_DDNS_OFF), P_CLOUDFLARE);
            return;
        }
        CloudflareSetup::Zone z;
        if (!_cf || zone.length() != 32 || !CloudflareSetup::find(zone.c_str(), z)) {
            _errorPage(S_UNKNOWN_ZONE, P_CLOUDFLARE);
            return;
        }
        // A bare label gets the domain after it; the name ends up lowercase
        char name[sizeof(_cfg.cf_host)];
        const char* s = in.c_str();
        size_t n = in.length(), zn = strlen(z.name);
        bool label = !strchr(s, '.');
        size_t total = label ? n + 1 + zn : n;
        bool ok = n && total < sizeof(name) &&
                  strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == n;
        if (ok) {
            snprintf(name, sizeof(name), label ? "%s.%s" : "%s", s, z.name);
            for (char* c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
            ok = strcmp(name, z.name) == 0 ||
                 (total > zn + 1 && name[total - zn - 1] == '.' && strcmp(name + total - zn, z.name) == 0);
        }
        if (!ok) {
            _errorPage(PSTR(L_INVALID_RECORD_NAME), P_CLOUDFLARE);
            return;
        }
        _cf->record(z.id, name);
        _go("/cloudflare");
    }

    // An API token: letters, digits, _ and -
    static bool _tokenText(const char* s) {
        for (; *s; s++)
            if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return false;
        return true;
    }

    void _handleNtfy() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR(L_NTFY_TITLE));

        if (_cfg.ntfy_topic[0])
            _alert(S_SUCCESS, String(L_TOPIC " ") + _cfg.ntfy_topic);
        else
            _alertP(S_WARNING, PSTR(L_TOPIC_NOT_SET));
        if (_cfg.ntfy_cmd[0] && strlen(_cfg.ntfy_cmd) < MIN_CMD_TOPIC && !_cfg.ntfy_token[0])
            _alertP(S_WARNING, PSTR(L_SHORT_TOPIC_WARN));
        if (_ntfy) {
            _box(PSTR(L_NTFY_COMMANDS));
            _row(S_STATE, _ntfy->commandStatus());
            _boxEnd();
        }

        _form(P_NTFY_SAVE);
        _input(PSTR(L_NTFY_HOST), S_F_HOST, _cfg.ntfy_host, PSTR("maxlength='63' autocapitalize='none' placeholder='ntfy.sh'"));
        _p(PSTR("<p class='m'>" L_NTFY_HOST_HELP "</p>"));
        _input(PSTR(L_NTFY_TOPIC), PSTR("topic"), _cfg.ntfy_topic,
               PSTR("maxlength='63' pattern='[A-Za-z0-9_\\-]*' placeholder='my-secret-topic'"));
        _p(PSTR("<p class='m'>" L_NTFY_TOPIC_HELP "</p>"));
        _input(PSTR(L_COMMAND_TOPIC), PSTR("cmd"), _cfg.ntfy_cmd,
               PSTR("id='cmd' maxlength='63' pattern='[A-Za-z0-9_\\-]*'"));
        _p(PSTR("<button type='button' class='b2' onclick=\"var a=new Uint8Array(12);"
                "crypto.getRandomValues(a);document.getElementById('cmd').value='cmd-'+"
                "Array.from(a,function(b){return('0'+b.toString(16)).slice(-2)}).join('')\">"
                L_GEN_RANDOM "</button><p class='m'>" L_CMD_TOPIC_HELP "</p>"));
        _passwordInput(PSTR(L_NTFY_TOKEN " <span class='m'>" L_OPTIONAL_KEEPS "</span>"), S_F_TOKEN,
                       PSTR("maxlength='47' placeholder='tk_...'"));
        _switch(PSTR("token_clear"), PSTR(L_DELETE_TOKEN), false);
        // With remote access on, how often the Worker checks the command topic
        _selectOpen(PSTR(L_POLL), PSTR("poll"));
        for (uint8_t s : POLLS) {
            char v[4];
            snprintf(v, sizeof(v), "%u", (unsigned)s);
            _optionOpen(v, s == _cfg.ntfy_poll);
            _s(v);
            _p(PSTR(" s</option>"));
        }
        _selectEnd();
        _p(PSTR("<p class='m'>" L_POLL_HELP "</p>"));
        _button(S_SAVE);
        _pageEnd();
    }

    void _handleNtfySave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String host = _server.arg("host");
        String topic = _server.arg("topic");
        String cmd = _server.arg("cmd");
        String token = _server.arg("token");
        host.trim();
        host.toLowerCase();
        topic.trim();
        cmd.trim();
        token.trim();
        if (host.length() > 0 && !NtfyNotifier::isValidHost(host.c_str())) {
            _errorPage(S_INVALID, P_NTFY);
            return;
        }
        if (topic.length() > 0 && !NtfyNotifier::isValidTopic(topic)) {
            _errorPage(PSTR(L_INVALID_TOPIC), P_NTFY);
            return;
        }
        if (cmd.length() > 0 && !NtfyNotifier::isValidTopic(cmd)) {
            _errorPage(PSTR(L_INVALID_CMD_TOPIC),
                       P_NTFY);
            return;
        }
        // ntfy.sh topics are public by name: without a token, the command topic
        // must be long enough not to be guessed
        bool hasToken = token.length() > 0 || (_cfg.ntfy_token[0] && !_flag("token_clear"));
        if (cmd.length() > 0 && cmd.length() < MIN_CMD_TOPIC && !hasToken) {
            _errorPage(PSTR(L_SHORT_TOPIC),
                       P_NTFY);
            return;
        }
        // The reply goes to the data topic: the same topic would read its own reply
        if (cmd.length() > 0 && cmd == topic) {
            _errorPage(PSTR(L_SAME_TOPIC), P_NTFY);
            return;
        }
        bool tokenOk = token.length() < sizeof(_cfg.ntfy_token);
        for (size_t i = 0; tokenOk && i < token.length(); i++) {
            char c = token.c_str()[i];
            tokenOk = isalnum((unsigned char)c) || c == '_';
        }
        if (!tokenOk) {
            _errorPage(PSTR(L_INVALID_NTFY_TOKEN), P_NTFY);
            return;
        }
        // The interval: one of the offered ones; missing keeps the current one
        String poll = _server.arg("poll");
        uint8_t every = _cfg.ntfy_poll;
        if (poll.length()) {
            bool offered = false;
            for (uint8_t s : POLLS) {
                char v[4];
                snprintf(v, sizeof(v), "%u", (unsigned)s);
                if (strcmp(poll.c_str(), v) == 0) {
                    every = s;
                    offered = true;
                }
            }
            if (!offered) {
                _errorPage(S_INVALID, P_NTFY);
                return;
            }
        }
        _cfg.ntfy_poll = every;
        bool moved = !host.equals(_cfg.ntfy_host) || !cmd.equals(_cfg.ntfy_cmd);
        strlcpy(_cfg.ntfy_host, host.c_str(), sizeof(_cfg.ntfy_host));
        strlcpy(_cfg.ntfy_topic, topic.c_str(), sizeof(_cfg.ntfy_topic));
        strlcpy(_cfg.ntfy_cmd, cmd.c_str(), sizeof(_cfg.ntfy_cmd));
        if (_flag("token_clear")) _cfg.ntfy_token[0] = '\0';
        else if (token.length()) strlcpy(_cfg.ntfy_token, token.c_str(), sizeof(_cfg.ntfy_token));
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_NTFY);
            return;
        }
        if (moved && _ntfy) _ntfy->restart();
        _savedPage(PSTR(L_NTFY_SAVED), P_NTFY);
    }

    // ── Remote access ────────────────────────────────────────────────────────

    // A DNS label: lowercase letters, digits and dashes, not at the ends
    static bool _label(const char* s, size_t max) {
        size_t n = strlen(s);
        return n && n <= max && s[0] != '-' && s[n - 1] != '-' &&
               strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789-") == n;
    }

    void _handleRemoteSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        String name = _server.arg("name");
        String zone = _server.arg("zone");
        String wd = _server.arg("wd");
        name.trim();
        wd.trim();
        if (!_label(name.c_str(), sizeof(_cfg.relay_name) - 1) || (wd.length() && !_label(wd.c_str(), 63))) {
            _errorPage(PSTR(L_INVALID_REMOTE_NAME), P_CLOUDFLARE);
            return;
        }
        CloudflareSetup::Zone z;
        if (!_cf || (zone.length() && (zone.length() != 32 || !CloudflareSetup::find(zone.c_str(), z)))) {
            _errorPage(S_UNKNOWN_ZONE, P_CLOUDFLARE);
            return;
        }
        _cf->deploy(name.c_str(), zone.c_str(), wd.c_str());
        _go("/cloudflare");
    }

    // Off: the device stops connecting; the Worker stays in the account
    // (with no device behind it, it answers that the device is offline)
    void _handleRemoteOff() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _relayOff();
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_CLOUDFLARE);
            return;
        }
        _savedPage(PSTR(L_REMOTE_OFF_DONE), P_CLOUDFLARE);
    }

    void _relayOff() {
        _cfg.relay_host[0] = '\0';
        _cfg.relay_secret[0] = '\0';
        _cfg.relay_ver = 0;
    }

    // Unregister: the token and everything it set up leave the device (DDNS
    // and the relay stop); the Worker and the record stay in the account.
    // i18n.js has asked for confirmation.
    void _handleCloudflareClear() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _relayOff();
        _cfg.relay_name[0] = '\0';
        _cfg.relay_zone[0] = '\0';
        _cfg.cf_token[0] = '\0';
        _cfg.cf_zone[0] = '\0';
        _cfg.cf_record[0] = '\0';
        _cfg.cf_host[0] = '\0';
        _cfg.cf_account[0] = '\0';
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_CONFIG);
            return;
        }
        LittleFS.remove(CloudflareSetup::ZONES_PATH);
        _log.log(PSTR("Cloudflare: token and settings removed"));
        _savedPage(PSTR(L_CF_CLEARED), P_CONFIG);
    }

    // ── MQTT ─────────────────────────────────────────────────────────────────

    void _handleMqtt() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("MQTT"));
        if (_mqtt) {
            _box(S_CONNECTION);
            _row(S_STATE, _mqtt->status());
            _row(PSTR(L_TOPICS), String(_mqtt->base()) + "/state, /cmd, /avail");
            _boxEnd();
        }
        _form(P_MQTT_SAVE);
        _input(PSTR("Broker <span class='m'>" L_BROKER_HINT "</span>"), S_F_HOST,
               _cfg.mqtt_host, PSTR("maxlength='63' placeholder='192.168.1.10:1883'"));
        _input(S_USER, S_F_USER, _cfg.mqtt_user,
               PSTR("maxlength='31' autocomplete='off'"));
        _passwordInput(PSTR(L_PASSWORD " <span class='m'>" L_BLANK_KEEPS "</span>"), S_F_PASS,
                       S_MAX63);
        _switch(PSTR("pass_clear"), PSTR(L_DELETE_PASSWORD), false);
        _p(PSTR("<p class='m'>" L_MQTT_HELP "</p>"));
        _button(S_SAVE);
        _pageEnd();
    }

    void _handleMqttSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String host = _server.arg("host");
        String user = _server.arg("user");
        String pass = _server.arg("pass");
        host.trim();
        user.trim();
        char h[sizeof(_cfg.mqtt_host)];
        uint16_t port;
        if (host.length() && !MqttService::parseBroker(host.c_str(), h, sizeof(h), port)) {
            _errorPage(PSTR(L_INVALID_BROKER), P_MQTT);
            return;
        }
        if (user.length() >= sizeof(_cfg.mqtt_user) || pass.length() >= sizeof(_cfg.mqtt_pass)) {
            _errorPage(PSTR(L_USER_PASS_LONG), P_MQTT);
            return;
        }
        strlcpy(_cfg.mqtt_host, host.c_str(), sizeof(_cfg.mqtt_host));
        strlcpy(_cfg.mqtt_user, user.c_str(), sizeof(_cfg.mqtt_user));
        if (_flag("pass_clear")) _cfg.mqtt_pass[0] = '\0';
        else if (pass.length()) strlcpy(_cfg.mqtt_pass, pass.c_str(), sizeof(_cfg.mqtt_pass));
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_MQTT);
            return;
        }
        if (_mqtt) _mqtt->reconnect();
        _savedPage(PSTR(L_MQTT_SAVED), P_MQTT);
    }

    void _handlePassword() {
        if (!_guard(true)) return;
        _pageBegin();
        _title(S_PANEL_PASSWORD);
        if (_cfg.webpss.length() == 0)
            _alertP(S_WARNING, PSTR(L_SET_PASSWORD));
        _passwordForm();
        if (_cfg.webpss.length()) {
            _p(PSTR("<h2>" L_MFA_REPLACE "</h2>"));
            _mfaResetForm();
        }
        _pageEnd();
    }

    // The password forms: on their own page for the first setup, and on Settings
    void _passwordForm() {
        _form(P_PASSWORD_SAVE);
        _input(S_USER, S_F_USER, _cfg.webusr.length() ? _cfg.webusr.c_str() : "admin",
               S_REQ32);
        _passwordInput(PSTR(L_NEW_PASS_MIN), S_F_PASS1, S_PASS8);
        _passwordInput(S_CONFIRM_PASS, S_F_PASS2, S_PASS8);
        if (_cfg.webpss.length()) {
            _passwordInput(S_CURRENT_PASS, S_F_PASS, S_REQ64,
                           S_CURRENT_PASS_AC);
            _codeInput();
        }
        _button(S_SAVE);
    }

    void _mfaResetForm() {
        _form(P_MFA_RESET);
        _p(PSTR("<p class='m'>" L_MFA_REPLACE_HELP "</p>"));
        _passwordInput(S_CURRENT_PASS, S_F_PASS, S_REQ64,
                       S_CURRENT_PASS_AC);
        _codeInput();
        _button(S_MFA_REPLACE);
    }

    // Changing a password that exists asks for it and for a code (_confirmed)
    void _handlePasswordSave() {
        if (!_guard(true)) return;
        if (!_checkCSRF()) return;
        if (_cfg.webpss.length() && !_confirmed()) return;
        if (!_readPanelCredentials("/password")) return;
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_PASSWORD);
            return;
        }
        _generateCSRF(); // rotates CSRF after a credentials change
        _clearSession(); // the old cookie must not survive a new password
        _savedPage(PSTR(L_PASSWORD_UPDATED), P_LOGIN);
    }

    // ── Sign-in and authenticator ────────────────────────────────────────────

    void _loginPage(PGM_P err, bool warn) {
        _pageBegin(0, err && !warn ? 401 : 200);
        _title(S_SIGN_IN);
        if (err) _alertP(warn ? S_WARNING : S_DANGER, err);
        else if (!_mfaOn()) _alertP(S_WARNING, PSTR(L_MFA_NEXT));
        _form(P_LOGIN, nullptr, true);
        _input(S_USER, S_F_USER, _cfg.webusr.c_str(),
               PSTR("required maxlength='32' autocomplete='username'"));
        _passwordInput(PSTR(L_PASSWORD), S_F_PASS, S_REQ64,
                       S_CURRENT_PASS_AC);
        if (_mfaOn()) _codeInput();
        _button(S_SIGN_IN);
        _pageEnd();
    }

    void _showKey() {
        char b32[33], grp[40];
        Totp::toBase32(_pending, b32);
        int n = 0;
        for (int i = 0; i < 32; i++) {
            if (i && (i & 3) == 0) grp[n++] = ' ';
            grp[n++] = b32[i];
        }
        grp[n] = '\0';
        _p(PSTR("<p class='k'>"));
        _s(grp);
        _p(PSTR("</p>"));
    }

    void _mfaPage(PGM_P err, bool warn) {
        _ensurePending();
        _pageBegin(0, err && !warn ? 401 : 200);
        _title(PSTR(L_MFA_SETUP));
        if (err) _alertP(warn ? S_WARNING : S_DANGER, err);
        _p(PSTR("<p class='m'>" L_MFA_HELP "</p>"));
        _showKey();
        _form(P_MFA);
        _codeInput();
        _button(S_SAVE);
        _pageEnd();
    }

    void _handleLogin() {
        if (_cfg.webpss.length() == 0) {
            _redirect("/password");
            return;
        }
        // cppcheck-suppress knownConditionTrueFalse ; the counter changes between requests
        if (_loginBlocked()) return;
        if (_sessionOk()) {
            _redirect(_session == 2 ? "/" : "/mfa");
            return;
        }
        _loginPage(nullptr, false);
    }

    void _handleLoginPost() {
        if (_cfg.webpss.length() == 0) {
            _redirect("/password");
            return;
        }
        // cppcheck-suppress knownConditionTrueFalse ; the counter changes between requests
        if (_loginBlocked()) return;
        if (!_checkCSRF(true)) return;
        bool mfa = _mfaOn();
        CodeTime t = mfa ? _codeTime() : CodeTime();
        // No time at all (no clock and no browser time): every attempt looks
        // the same, and it does not count. Otherwise a wrong password could
        // be told apart from a code that cannot be checked.
        if (mfa && !t.now) {
            _loginPage(S_CLOCK, true);
            return;
        }
        String user = _server.arg("user");
        user.trim();
        String pass = _server.arg("pass");
        bool passOk = _credentialOk(user, pass);
        uint32_t step = _floor(t);
        bool codeOk = !mfa || _codeOk(_cfg.totp, t.now, step);
        if (!passOk || !codeOk) {
            _loginFailed(_tries());
            _loginPage(S_SIGN_IN_FAILED, false);
            return;
        }
        _signedIn();
        if (mfa) {
            // The code is spent, also after a reboot. A failed save keeps it
            // in RAM until the next one.
            _spend(t, step);
            _store.save();
        }
        // A key shown to an earlier session is not offered again
        _pendingSet = false;
        _startSession(mfa ? 2 : 1);
        _log.log(PSTR("Panel: signed in"));
        _redirect(mfa ? "/" : "/mfa");
    }

    // Without the session there is nothing to close, so another device on
    // the network cannot sign the owner out with the public sign-in token.
    void _handleLogout() {
        if (_sessionOk()) {
            if (!_checkCSRF()) return;
            _clearSession();
        }
        _redirect("/login");
    }

    void _handleMfa() {
        if (!_enrollGuard()) return;
        _mfaPage(nullptr, false);
    }

    // Enrollment, or the second half of a replacement: the new key goes into
    // the config only after one of its codes, so the current key (if any)
    // keeps working until then.
    void _handleMfaPost() {
        if (!_enrollGuard()) return;
        if (!_checkCSRF()) return;
        CodeTime t = _codeTime();
        if (!t.now) {
            _mfaPage(S_CLOCK, true);
            return;
        }
        uint32_t step = _floor(t);
        if (!_pendingSet || !_codeOk(_pending, t.now, step)) {
            _loginFailed(_tries());
            _mfaPage(S_SIGN_IN_FAILED, false);
            return;
        }
        uint8_t prev[Totp::KEY_LEN];
        uint32_t prevStep = _cfg.totpStep, prevBrowser = _cfg.totpBrowserStep;
        memcpy(prev, _cfg.totp, Totp::KEY_LEN);
        memcpy(_cfg.totp, _pending, Totp::KEY_LEN);
        _spend(t, step);
        if (!_store.save()) {
            memcpy(_cfg.totp, prev, Totp::KEY_LEN);
            _cfg.totpStep = prevStep;
            _cfg.totpBrowserStep = prevBrowser;
            _errorPage(S_SAVE_FAILED, P_MFA);
            return;
        }
        _pendingSet = false;
        _startSession(2);
        _signedIn();
        _log.log(PSTR("Panel: authenticator on"));
        _redirect("/");
    }

    // Current password and code, then a new key to confirm on /mfa. Nothing
    // is changed in the config here: leaving /mfa (signing out, the session
    // ending, a reboot) keeps the current key.
    void _handleMfaReset() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        if (!_confirmed()) return;
        _store.save();   // the code is spent; a failed save keeps it in RAM
        _pendingSet = false;
        _ensurePending();
        _startSession(1);
        _signedIn();
        _log.log(PSTR("Panel: authenticator reset"));
        _redirect("/mfa");
    }

    // Served straight from flash; the cache avoids resending on every page.
    // CSS, JS and the pinout carry the content hash in the URL (hashed): a
    // new version is another URL, so the browser keeps them for a year
    // without asking again.
    // The type is in RAM: it goes into the header's %s, which does not read
    // from flash. The photo is AVIF, already compressed: no gzip.
    void _asset(const char* type, const uint8_t* data, size_t len) {
        _server.sendHeader("Cache-Control", "max-age=31536000, immutable");
        _server.sendHeader("Content-Encoding", "gzip");
        _server.send_P(200, type, reinterpret_cast<PGM_P>(data), len);
    }

    // "UTC-03:00 Brasilia, Buenos Aires", with the saved zone selected
    void _tzSelect() {
        _selectOpen(PSTR(L_TIME_ZONE " <span class='m'>" L_NO_DST "</span>"), PSTR("tz"));
        PGM_P place = _TZ_PLACES;
        for (size_t i = 0; i < sizeof(_TZ_MIN) / sizeof(_TZ_MIN[0]); i++) {
            int16_t tz = (int16_t)pgm_read_word(&_TZ_MIN[i]);
            int a = tz < 0 ? -tz : tz;
            char b[24];
            snprintf(b, sizeof(b), "%d", tz);
            _optionOpen(b, tz == _cfg.tz);
            snprintf(b, sizeof(b), "UTC%c%02d:%02d ", tz < 0 ? '-' : '+', a / 60, a % 60);
            _s(b);
            _p(place);
            _p(S_OPTION_END);
            place += strlen_P(place) + 1;
        }
        _selectEnd();
    }

    // "HH:MM" from a minute of the day, into b (8 bytes or more)
    static const char* _hhmm(uint16_t m, char* b) {
        snprintf_P(b, 8, PSTR("%02u:%02u"), m / 60u, m % 60u);
        return b;
    }

    const char* _localTime(char* b) const {
        uint16_t m;
        return _iot.localMinute(m) ? _hhmm(m, b) : L_CLOCK_NOT_SYNC;
    }

    // "HH:MM" (time field) to a minute of the day
    static bool parseHhmm(const String& s, uint16_t& out) {
        const char* p = s.c_str();
        if (s.length() != 5 || p[2] != ':') return false;
        for (int i : {0, 1, 3, 4})
            if (p[i] < '0' || p[i] > '9') return false;
        int h = (p[0] - '0') * 10 + (p[1] - '0');
        int m = (p[3] - '0') * 10 + (p[4] - '0');
        if (h > 23 || m > 59) return false;
        out = (uint16_t)(h * 60 + m);
        return true;
    }

public:
    // One of the official zones in the list
    static bool validTz(int16_t tz) {
        for (size_t i = 0; i < sizeof(_TZ_MIN) / sizeof(_TZ_MIN[0]); i++)
            if ((int16_t)pgm_read_word(&_TZ_MIN[i]) == tz) return true;
        return false;
    }

private:
    // "how long ago" from a millis(); "never" if it did not happen. The
    // duration goes after the marker: the language puts it in its place
    static const char* _ago(bool happened, unsigned long at, char* b) {
        if (!happened) return L_NEVER;
        unsigned long s = (millis() - at) / 1000;
        snprintf_P(b, DURATION_LEN, PSTR(L_AGO "%luh %02lum %02lus"), s / 3600, (s / 60) % 60, s % 60);
        return b;
    }

    void _go(const char* href) {
        _server.sendHeader("Location", href);
        _server.send(303, "text/plain", "");
    }

    void _handleUpdate() {
        if (!_guard()) return;
        _pageBegin(_ota.pending() ? 3 : 0);
        _otaProgressBar();

        _title(PSTR(L_UPDATE));
        _box(PSTR("Firmware"));
        _row(PSTR(L_CURRENT_VERSION), firmware_version);
        _row(PSTR(L_AUTOMATIC_CAP), _cfg.ota_auto ? L_ON : L_OFF);
        if (_ota.availableVersion()[0] != '\0')
            _row(PSTR(L_AVAILABLE), _ota.availableVersion());
        if (_ota.detail()[0] != '\0')
            _row(S_STATE, _ota.detail());
        _boxEnd();

        _form(P_UPDATE_AUTO);
        _switch(PSTR("ota_auto"), PSTR(L_AUTO_UPDATE), _cfg.ota_auto);
        _p(PSTR("<button class='b2'>" L_SAVE "</button></form>"));

        _form(P_UPDATE_CHECK);
        _button(PSTR(L_CHECK_NOW));

        if (_ota.availableVersion()[0] != '\0' && !_ota.inProgress()) {
            _form(P_UPDATE_INSTALL);
            _button(PSTR(L_INSTALL_VERSION));
        }
        _pageEnd();
    }

    void _handleUpdateAuto() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _cfg.ota_auto = _server.arg("ota_auto") == "1";
        _ota.setAutomatic(_cfg.ota_auto);
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_UPDATE);
            return;
        }
        _savedPage(_cfg.ota_auto
                       ? PSTR(L_AUTO_UPDATE_ON)
                       : PSTR(L_AUTO_UPDATE_OFF),
                   P_UPDATE);
    }

    void _handleUpdateCheck() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _ota.requestCheck();
        _go("/update");
    }

    void _handleUpdateInstall() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        if (_ota.availableVersion()[0] == '\0') {
            _errorPage(PSTR(L_NO_NEW_VERSION), P_UPDATE);
            return;
        }
        _ota.requestInstall();
        _go("/update");
    }

    // The module photo: AVIF on its white background (Assets.h)
    // ── IoT ──────────────────────────────────────────────────────────────────

    // <option value='value'> (escaped), selected or not; the caller writes the text
    void _optionOpen(const char* value, bool on) {
        _p(PSTR("<option value='"));
        _e(value);
        _p(on ? PSTR("' selected>") : S_TAG_END);
    }

    void _option(const char* value, PGM_P label, bool on) {
        _optionOpen(value, on);
        _p(label);
        _p(S_OPTION_END);
    }

    void _selectOpen(PGM_P label, PGM_P name) {
        _labelOpen(label);
        _p(PSTR("<select name='"));
        _p(name);
        _p(S_TAG_END);
    }

    void _selectEnd() { _p(PSTR("</select></label>")); }


    // none: the text for -1
    void _pinSelect(PGM_P label, PGM_P name, int8_t current, PGM_P none) {
        static const int8_t pins[] = {-1, 2, 0, 1, 3};
        static const char* const values[] = {"-1", "2", "0", "1", "3"};
        static const char* const labels[] = {
            nullptr, "GPIO2", "GPIO0", "TX (GPIO1)", "RX (GPIO3)"
        };
        _selectOpen(label, name);
        for (int i = 0; i < 5; i++) _option(values[i], i ? labels[i] : none, current == pins[i]);
        _selectEnd();
    }









    void _handleIot() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("IoT"));
        const IoTConfig& t = _cfg.iot;
        if (_iot.unknownType())
            _alertP(S_WARNING, PSTR(L_TYPE_UNKNOWN));
        if (t.notify && _cfg.ntfy_topic[0] == '\0')
            _alertP(S_WARNING, PSTR(L_SET_NTFY_TOPIC));

        // Something set up: Reconfigure clears it all (asks twice, with the
        // password and a code) for a new setup
        if ((t.sensor[0] && t.sensorPin >= 0) || (t.actuator[0] && t.actuatorPin >= 0)) {
            _form(P_IOT_CLEAR);
            _p(PSTR("<button class='b2'>" L_RECONFIGURE "</button></form>"));
        }

        // Drawing in English; i18n.js redraws it in the panel language
        _p(PSTR("<figure><img src='/pinout.svg?v=" ASSET_PINOUT_TAG "' width='1040' height='700' "
                "alt='" L_PINOUT_ALT "'></figure>"));

        // The fields come from iot.js, from the saved setup (CF, _setupScript)
        _form(P_IOT_SAVE, PSTR(" id='fm'"));
        _p(PSTR("</form>"));
        if (_iot.hasSensor() || _iot.hasActuator()) _apiBox();

        _pageFoot();
        _iotScript(true);
    }

    // iot.js and what it reads: type → Kind/Cap from the catalog, the CSRF
    // token, the saved rule [limit, value it watches] (NaN: no rule) and D:
    // the unit, the tank, the flow meter, the Pulse seconds and the
    // actuator's name (a text marker). Ends the page.
    void _iotScript(bool setup = false) {
        _p(PSTR("<script>var SK={"));
        for (uint8_t i = 0; i < iot::SENSOR_COUNT; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s%s:%u", i ? "," : "", iot::SENSORS[i].slug,
                     iot::SENSORS[i].kind);
            _s(buf);
        }
        _p(PSTR("},AC={"));
        for (uint8_t i = 0; i < iot::ACTUATOR_COUNT; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s%s:%u", i ? "," : "", iot::ACTUATORS[i].slug,
                     iot::ACTUATORS[i].cap);
            _s(buf);
        }
        _p(PSTR("},T='"));
        _s(_formToken());
        const IoTConfig& t = _cfg.iot;
        const iot::ActuatorDef* a = _iot.actuatorDef();
        char buf[96];
        if (_iot.hasRule()) snprintf(buf, sizeof(buf), "',R=[%d,%u", t.threshold, t.metric);
        else strcpy(buf, "',R=[NaN,0");
        _s(buf);
        unsigned pulse = t.pulse ? t.pulse : a ? a->pulse : 0;
        snprintf(buf, sizeof(buf), "],D={k:%u,c:%u,u:%u,e:%u,f:%u,l:%u,p:%u,s:%u,t:'%s',a:'", _iot.sensorKind(),
                 _iot.capability(), t.unit, t.tankEmpty, t.tankFull, t.tankL, t.ppl, pulse, a ? a->slug : "");
        _s(buf);
        if (a) _p(a->label);
        _p(PSTR("'}"));
        if (setup) _setupScript();
        _p(PSTR(";</script><script src='/iot.js?v=" WEB_IOT_TAG "'></script></body></html>"));
    }

    // The saved setup for the IoT page's form (iot.js), and each type's name
    // (a text marker). Texts go in their allowed characters only: nothing
    // else reaches the script (a type this version does not know keeps its
    // role on the form, as its name).
    void _setupScript() {
        const IoTConfig& t = _cfg.iot;
        char b[160];
        _p(PSTR(",CF={s:'"));
        _jsText(t.sensor);
        _p(PSTR("',a:'"));
        _jsText(t.actuator);
        snprintf_P(b, sizeof(b), PSTR("',sp:%d,sh:%d,m:%d,u:%d,te:%d,tf:%d,tl:%d,pp:%d,ap:%d,ah:%d,d1:%d,d0:%d,g1:%d,g0:%d,"),
                   t.sensorPin, t.sensorHigh, t.metric, t.unit, t.tankEmpty, t.tankFull, t.tankL, t.ppl, t.actuatorPin,
                   t.actuatorHigh, t.dutyOn, t.dutyOff, t.angleOn, t.angleOff);
        _s(b);
        snprintf_P(b, sizeof(b), PSTR("le:%d,mo:%d,pu:%d,re:%d,ru:%d,to:%d,ab:%d,th:%d,ma:%d,li:%d,hi:%d,wi:%d,w1:%d,w2:%d,"
                                      "no:%d,on:'"),
                   t.leds, t.maxOn, t.pulse, t.restore, t.rule, t.toggle, t.above, t.threshold, t.margin, t.linear,
                   t.thresholdHi, t.window, t.winStart, t.winEnd, t.notify);
        _s(b);
        _jsText(t.onText);
        _p(PSTR("',of:'"));
        _jsText(t.offText);
        _p(PSTR("'},SL={"));
        for (const iot::SensorDef& s : iot::SENSORS) _nameEntry(s.slug, s.label);
        _p(PSTR("},AL={"));
        for (const iot::ActuatorDef& a : iot::ACTUATORS) _nameEntry(a.slug, a.label);
        _p(PSTR("}"));
    }

    // slug:'name', in an object literal
    void _nameEntry(const char* slug, PGM_P label) {
        _s(slug);
        _p(PSTR(":'"));
        _p(label);
        _p(PSTR("',"));
    }

    // A name in a script string: only the characters the panel accepts
    void _jsText(const char* s) {
        char b[16];
        size_t n = 0;
        for (size_t i = 0, k = strnlen(s, sizeof(b) - 1); i < k; i++)
            if (isalnum((unsigned char)s[i]) || strchr(" -_.,", s[i])) b[n++] = s[i];
        b[n] = '\0';
        _s(b);
    }

    // Reads an integer from the form within the range; otherwise answers 400
    bool _int(const char* name, int min, int max, int16_t& out) {
        if (IoTService::parseI16(_server.arg(name), min, max, out)) return true;
        _errorPage(S_INVALID, P_IOT);
        return false;
    }

    bool _u8(const char* name, int min, int max, uint8_t& out) {
        int16_t v = 0;
        if (!_int(name, min, max, v)) return false;
        out = (uint8_t)v;
        return true;
    }

    bool _flag(const char* name) { return _server.arg(name) == "1"; }

    // State name: up to 15 letters, digits, space, - _ . ,
    bool _stateText(const char* name, char* dst) {
        String v = _server.arg(name);
        v.trim();
        bool ok = v.length() < 16;
        for (size_t i = 0; ok && i < v.length(); i++) {
            char c = v.c_str()[i];
            ok = isalnum((unsigned char)c) || strchr(" -_.,", c) != nullptr;
        }
        if (!ok) {
            _errorPage(PSTR(L_INVALID_STATE_NAME), P_IOT);
            return false;
        }
        strlcpy(dst, v.c_str(), 16);
        return true;
    }

    bool _pinArg(const char* name, int8_t& out) {
        String v = _server.arg(name);   // a temporary inside && counts one more branch
        if (IoTService::parsePin(v, out) && iot::allowedPin(out)) return true;
        _errorPage(PSTR(L_CHOOSE_PIN), P_IOT);
        return false;
    }

    void _handleIotSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String role = _server.arg("role");
        bool hs = role == "s" || role == "sa";
        bool ha = role == "a" || role == "sa";
        if (!hs && !ha && role != "n") {
            _errorPage(PSTR(L_CHOOSE_ROLE), P_IOT);
            return;
        }

        // A part not in use keeps its settings and loses only its type and pin
        IoTConfig t = _cfg.iot;
        t.sensor[0] = '\0';
        t.sensorPin = -1;
        t.actuator[0] = '\0';
        t.actuatorPin = -1;
        uint8_t kind = iot::K_NONE;
        uint8_t cap = iot::C_NONE;

        if (hs) {
            String slug = _server.arg("sensor");
            int8_t i = iot::sensorIndex(slug.c_str());
            if (i < 0) {
                _errorPage(PSTR(L_INVALID_SENSOR), P_IOT);
                return;
            }
            strlcpy(t.sensor, slug.c_str(), sizeof(t.sensor));
            kind = iot::SENSORS[i].kind;
            if (!_pinArg("sensor_pin", t.sensorPin)) return;
            if (kind == iot::K_DIGITAL) {
                t.sensorHigh = _flag("sensor_high");
                if (!_stateText("sensor_on", t.onText)) return;
                if (!_stateText("sensor_off", t.offText)) return;
            }
            if (iot::tempAndHumidity(kind) && !_u8("metric", 0, 1, t.metric)) return;
            // Units: two for a temperature, three for a distance or pulses
            t.unit = 0;
            if (iot::unitCount(kind) && !_u8("unit", 0, iot::unitCount(kind) - 1, t.unit)) return;
            if (kind == iot::K_RANGE && t.unit) {
                int16_t e = 0, f = 0, l = 0;
                if (!_int("tank_empty", 2, 400, e) || !_int("tank_full", 0, e - 1, f)) return;
                if (t.unit == 2 && !_int("tank_l", 1, 30000, l)) return;
                t.tankEmpty = (uint16_t)e;
                t.tankFull = (uint16_t)f;
                if (l) t.tankL = (uint16_t)l;
            }
            if (kind == iot::K_PULSE && t.unit) {
                int16_t p = 0;
                if (!_int("ppl", 1, 30000, p)) return;
                t.ppl = (uint16_t)p;
            }
        }

        if (ha) {
            String slug = _server.arg("actuator");
            int8_t i = iot::actuatorIndex(slug.c_str());
            if (i < 0) {
                _errorPage(PSTR(L_INVALID_ACTUATOR), P_IOT);
                return;
            }
            strlcpy(t.actuator, slug.c_str(), sizeof(t.actuator));
            cap = iot::ACTUATORS[i].cap;
            if (!_pinArg("actuator_pin", t.actuatorPin)) return;
            if (hs && (t.actuatorPin == t.sensorPin ||
                       (iot::i2c(kind) && t.actuatorPin == I2cBus::sclFor(t.sensorPin)))) {
                _errorPage(PSTR(L_SAME_PINS), P_IOT);
                return;
            }
            if (cap == iot::C_ONOFF) t.actuatorHigh = _flag("actuator_high");
            if (cap == iot::C_LEVEL &&
                (!_u8("duty_on", 0, 100, t.dutyOn) || !_u8("duty_off", 0, 100, t.dutyOff)))
                return;
            if (cap == iot::C_POSITION &&
                (!_u8("angle_on", 0, 180, t.angleOn) || !_u8("angle_off", 0, 180, t.angleOff)))
                return;
            if (cap == iot::C_COLOR && !_u8("leds", 1, IoTService::MAX_LEDS, t.leds)) return;
            int16_t maxOn = 0, pulse = 0;
            if (!_int("max_on", 0, 1440, maxOn) || !_int("pulse", 0, 3600, pulse)) return;
            t.maxOn = (uint16_t)maxOn;
            t.pulse = (uint16_t)pulse;
            t.restore = _flag("restore");
        }

        if (hs && ha) {
            t.rule = _flag("rule");
            if (t.rule && kind == iot::K_DIGITAL) t.toggle = _flag("toggle");
            if (t.rule && kind != iot::K_DIGITAL) {
                t.above = _flag("above");
                if (!_int("threshold", -999, 9999, t.threshold)) return;
                if (!_u8("margin", 0, 50, t.margin)) return;
                t.linear = cap == iot::C_LEVEL && _flag("linear");
                if (t.linear) {
                    if (!_int("threshold_hi", -999, 9999, t.thresholdHi)) return;
                    if (t.thresholdHi <= t.threshold) {
                        _errorPage(PSTR(L_HIGH_ABOVE), P_IOT);
                        return;
                    }
                }
            }
            t.window = t.rule && _flag("window");
            if (t.window) {
                String from = _server.arg("win_start");   // a temporary inside || counts one more branch
                String to = _server.arg("win_end");
                if (!parseHhmm(from, t.winStart) || !parseHhmm(to, t.winEnd)) {
                    _errorPage(PSTR(L_INVALID_TIME), P_IOT);
                    return;
                }
                if (t.winStart == t.winEnd) {
                    _errorPage(PSTR(L_WINDOW_SAME), P_IOT);
                    return;
                }
            }
        }
        t.notify = _flag("notify");
        if (!PowerService::pinsFree(_cfg.bat_sda, t)) {
            _errorPage(PSTR(L_PIN_GAUGE), P_IOT);
            return;
        }
        if ((hs || ha) && !_cfg.api_token[0]) _newToken();

        _cfg.iot = t;
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_IOT);
            return;
        }
        _iot.begin();
        _savedPage(PSTR(L_IOT_SET), P_IOT);
    }

    // State as JSON, or the command error
    void _sendState(uint8_t result) {
        _noStore();
        if (result != IoTService::OK) {
            const char* msg = result == IoTService::INVALID ? "invalid command"
                            : result == IoTService::UNSUPPORTED ? "command not valid for this actuator"
                            : "no actuator configured";
            _jsonError(400, msg);
            return;
        }
        char buf[STATE_JSON_LEN];
        _iot.stateJson(buf, sizeof(buf));
        _server.send(200, "application/json", buf);
    }

    void _handleIotNow() {
        if (!_guard()) return;
        _sendState(IoTService::OK);
    }

    // Panel command (the same text as the API and ntfy)
    void _handleIotCmd() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        String cmd = _server.arg("cmd");
        _log.log(PSTR("Panel: command \"%.32s\""), cmd.c_str());
        _sendState(_iot.command(cmd.c_str(), millis()));
    }

    void _jsonError(int code, const char* msg) {
        _server.send(code, "application/json", String("{\"error\":\"") + msg + "\"}");
    }

    // 32 hex digits from the hardware RNG
    void _newToken() {
        for (int i = 0; i < 4; i++)
            snprintf(_cfg.api_token + i * 8, 9, "%08lx", (unsigned long)ESP.random());
    }

    // Authorization = scheme + secret. Constant-time comparison: the response
    // time does not reveal the prefix.
    bool _authIs(const char* scheme, const char* secret) {
        String auth = _server.authorization();
        size_t s = strlen(scheme), n = strlen(secret);
        if (n == 0 || auth.length() != s + n || strncmp(auth.c_str(), scheme, s) != 0)
            return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < n; i++) diff |= (uint8_t)(auth.c_str()[s + i] ^ secret[i]);
        return diff == 0;
    }

    bool _bearerOk() { return _authIs("Bearer ", _cfg.api_token); }

    // The API only exists with a sensor or actuator configured
    bool _apiGuard() {
        _noStore();
        if (!_iot.hasSensor() && !_iot.hasActuator()) {
            _jsonError(404, "no sensor or actuator configured");
            return false;
        }
        // cppcheck-suppress knownConditionTrueFalse ; the counter changes between requests
        if (_blocked(_apiTries)) return false;
        if (!_bearerOk()) {
            if (_server.authorization().length()) _loginFailed(_apiTries);   // a probe without one is no guess
            _server.sendHeader("WWW-Authenticate", "Bearer");
            _jsonError(401, "invalid token");
            return false;
        }
        _apiTries.fails = 0;
        return true;
    }

    void _handleApiState() {
        if (!_apiGuard()) return;
        _sendState(IoTService::OK);
    }

    // Plain-text body (curl -d on) or the cmd form field
    void _handleApiCmd() {
        if (!_apiGuard()) return;
        String cmd = _server.arg("cmd");
        if (cmd.length() == 0) cmd = _server.arg("plain");
        _log.log(PSTR("API: command \"%.32s\""), cmd.c_str());
        _sendState(_iot.command(cmd.c_str(), millis()));
    }

    void _handleIotToken() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _newToken();
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_IOT);
            return;
        }
        _savedPage(PSTR(L_NEW_TOKEN_DONE), P_IOT);
    }

    // Deletes sensor, actuator, rule and token. Asks first on its own page
    // (a plain form that the screen reader announces), then in the browser;
    // with a password set, the current password and a code confirm it, as
    // for a new password: the session cookie alone is not enough.
    void _handleIotClear() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        bool locked = _cfg.webpss.length() > 0;
        if (!_flag("confirm")) {
            _pageBegin();
            _title(PSTR(L_RECONFIGURE));
            _alertP(S_WARNING,
                    PSTR(L_REMOVE_WARNING));
            _form(P_IOT_CLEAR, PSTR(" data-q='" L_IOT_CLEAR_ASK "'"));
            if (locked) {
                _passwordInput(S_CURRENT_PASS, S_F_PASS, S_REQ64, S_CURRENT_PASS_AC);
                _codeInput();
            }
            _p(PSTR("<input type='hidden' name='confirm' value='1'>"
                    "<button class='bx'>" L_YES_CLEAR "</button></form>"));
            _backButton(P_IOT);
            _pageEnd();
            return;
        }
        if (locked && !_confirmed(P_IOT)) return;
        _cfg.iot = IoTConfig();
        _cfg.api_token[0] = '\0';
        if (!_store.save()) {
            _errorPage(S_SAVE_FAILED, P_IOT);
            return;
        }
        LittleFS.remove(IoTState::PATH);
        _iot.begin();
        _log.log(PSTR("IoT: sensor and actuator removed"));
        _savedPage(PSTR(L_IOT_CLEARED), P_IOT);
    }

    // Token and examples to integrate without internet (scripts, Home Assistant)
    void _apiBox() {
        String ip = WiFi.localIP().toString();
        _box(PSTR(L_LOCAL_API));
        _p(PSTR("<div class='r'><span>" L_API_HELP "</span></div>"));
        if (_cfg.api_token[0]) {
            _p(PSTR("<div class='r'><label>" L_TOKEN "<input readonly value='"));
            _s(_cfg.api_token);
            _p(PSTR("' onclick='this.select()'></label></div><pre>curl -H 'Authorization: Bearer "));
            _s(_cfg.api_token);
            _p(PSTR("' http://"));
            _s(ip);
            _p(PSTR("/api/state\ncurl -H 'Authorization: Bearer "));
            _s(_cfg.api_token);
            _p(PSTR("' -d on http://"));
            _s(ip);
            _p(PSTR("/api/cmd</pre>"));
        }
        _boxEnd();
        _form(P_IOT_TOKEN);
        _p(_cfg.api_token[0] ? PSTR("<button class='b2'>" L_GEN_NEW_TOKEN "</button></form>")
                             : PSTR("<button class='b2'>" L_GEN_TOKEN "</button></form>"));
    }
};

// Defined after the class: they point at its handlers
inline const WebService::Route WebService::ROUTES[] PROGMEM = {
    {HttpServer::GET, P_ROOT, [](WebService& w) { w._handleRoot(); }},
    {HttpServer::GET, P_LOG, [](WebService& w) { w._handleLog(); }},
    {HttpServer::GET, P_STATUS, [](WebService& w) { w._handleStatus(); }},
    {HttpServer::GET, P_CLOUDFLARE, [](WebService& w) { w._handleCloudflare(); }},
    {HttpServer::GET, P_NTFY, [](WebService& w) { w._handleNtfy(); }},
    {HttpServer::GET, P_MQTT, [](WebService& w) { w._handleMqtt(); }},
    {HttpServer::GET, P_PASSWORD, [](WebService& w) { w._handlePassword(); }},
    {HttpServer::GET, P_LOGIN, [](WebService& w) { w._handleLogin(); }},
    {HttpServer::GET, P_MFA, [](WebService& w) { w._handleMfa(); }},
    {HttpServer::GET, P_IOT, [](WebService& w) { w._handleIot(); }},
    {HttpServer::GET, P_CONFIG, [](WebService& w) { w._handleConfig(); }},
    {HttpServer::GET, P_IOT_NOW, [](WebService& w) { w._handleIotNow(); }},
    {HttpServer::GET, P_API_STATE, [](WebService& w) { w._handleApiState(); }},
    {HttpServer::GET, P_UPDATE, [](WebService& w) { w._handleUpdate(); }},
    {HttpServer::POST, P_SAVE, [](WebService& w) { w._handleSave(); }},
    {HttpServer::POST, P_CONFIG_SAVE, [](WebService& w) { w._handleConfigSave(); }},
    {HttpServer::POST, P_CLOUDFLARE_SAVE, [](WebService& w) { w._handleCloudflareSave(); }},
    {HttpServer::POST, P_CLOUDFLARE_DDNS, [](WebService& w) { w._handleCloudflareDdns(); }},
    {HttpServer::POST, P_CLOUDFLARE_CLEAR, [](WebService& w) { w._handleCloudflareClear(); }},
    {HttpServer::POST, P_NTFY_SAVE, [](WebService& w) { w._handleNtfySave(); }},
    {HttpServer::POST, P_MQTT_SAVE, [](WebService& w) { w._handleMqttSave(); }},
    {HttpServer::POST, P_REMOTE_SAVE, [](WebService& w) { w._handleRemoteSave(); }},
    {HttpServer::POST, P_REMOTE_OFF, [](WebService& w) { w._handleRemoteOff(); }},
    {HttpServer::POST, P_PASSWORD_SAVE, [](WebService& w) { w._handlePasswordSave(); }},
    {HttpServer::POST, P_LOGIN, [](WebService& w) { w._handleLoginPost(); }},
    {HttpServer::POST, P_LOGOUT, [](WebService& w) { w._handleLogout(); }},
    {HttpServer::POST, P_MFA, [](WebService& w) { w._handleMfaPost(); }},
    {HttpServer::POST, P_MFA_RESET, [](WebService& w) { w._handleMfaReset(); }},
    {HttpServer::POST, P_IOT_SAVE, [](WebService& w) { w._handleIotSave(); }},
    {HttpServer::POST, P_IOT_CMD, [](WebService& w) { w._handleIotCmd(); }},
    {HttpServer::POST, P_IOT_TOKEN, [](WebService& w) { w._handleIotToken(); }},
    {HttpServer::POST, P_IOT_CLEAR, [](WebService& w) { w._handleIotClear(); }},
    {HttpServer::POST, P_API_CMD, [](WebService& w) { w._handleApiCmd(); }},
    {HttpServer::POST, P_UPDATE_AUTO, [](WebService& w) { w._handleUpdateAuto(); }},
    {HttpServer::POST, P_UPDATE_CHECK, [](WebService& w) { w._handleUpdateCheck(); }},
    {HttpServer::POST, P_UPDATE_INSTALL, [](WebService& w) { w._handleUpdateInstall(); }},
};

inline const WebService::Asset WebService::ASSETS[] PROGMEM = {
    {P_S_CSS, "text/css", WEB_CSS_GZ, sizeof(WEB_CSS_GZ)},
    {P_IOT_JS, "text/javascript", WEB_IOT_GZ, sizeof(WEB_IOT_GZ)},
    {P_I18N_JS, "text/javascript", WEB_I18N_GZ, sizeof(WEB_I18N_GZ)},
    {P_PINOUT_SVG, "image/svg+xml", ASSET_PINOUT_GZ, sizeof(ASSET_PINOUT_GZ)},
};

inline const size_t WebService::ROUTE_COUNT = sizeof(ROUTES) / sizeof(ROUTES[0]);
inline const size_t WebService::ASSET_COUNT = sizeof(ASSETS) / sizeof(ASSETS[0]);
