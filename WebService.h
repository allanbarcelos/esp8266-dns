// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "HttpServer.h"
#include <LittleFS.h>
#include "Clock.h"
#include "Config.h"
#include "Totp.h"
#include "Logger.h"
#include "Version.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "OTAService.h"
#include "IoTService.h"
#include "NtfyNotifier.h"
#include "MqttService.h"
#include "PowerService.h"
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

static const char _HTML_HEAD_C[] PROGMEM =
    "<nav aria-label='" L_MENU "'><a href='/'>" L_HOME "</a><a href='/status'>" L_STATUS "</a>"
    "<a href='/log'>" L_LOGS "</a><a href='/cloudflare'>Cloudflare</a><a href='/ntfy'>Ntfy</a><a href='/mqtt'>MQTT</a>"
    "<a href='/iot'>IoT</a><a href='/config'>" L_SETTINGS "</a><a href='/update'>" L_UPDATE "</a>"
    "<a href='/password'>" L_PASSWORD "</a>";

static const char _HTML_FOOT_A[] PROGMEM = "</main><footer>Firmware: ";

// Attributes for the 6-digit authenticator field. Not a class member:
// C++11 cannot define a non-integral static inside the class.
static const char _CODE_FIELD[] PROGMEM =
    "required inputmode='numeric' pattern='[0-9]{6}' minlength='6' maxlength='6' autocomplete='one-time-code'";

// The panel language goes after the #: the browser does not send it, so
// the cache keeps a single copy of i18n.js
static const char _HTML_FOOT_B[] PROGMEM =
    "</footer><script src='/i18n.js?v=" WEB_I18N_TAG "#";

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
               PowerService* power = nullptr)
        : _server(80), _cfg(cfg), _log(log), _store(store),
          _wifi(wifi), _ota(ota), _iot(iot), _ntfy(ntfy), _mqtt(mqtt), _power(power) {
        _csrfToken[0] = '\0';
    }

    void begin() {
        _generateCSRF();
        _server.begin(_dispatch, this);
        _log.log(PSTR("Web server listening on port 80"));
    }

    void tick() { _server.handleClient(); }

    // Boot token (sign-in and first setup) and the token of the forms of
    // the current session (the boot one while there is none)
    const char* csrfToken() const { return _csrfToken; }
    const char* sessionCsrfToken() const { return _session ? _csrfSid : _csrfToken; }

    // Escapes text for safe use in HTML content and attributes
    static String escape(const String& s) {
        String out;
        for (size_t i = 0; i < s.length(); i++) {
            char c = s.c_str()[i];
            switch (c) {
                case '&':  out += "&amp;";  break;
                case '<':  out += "&lt;";   break;
                case '>':  out += "&gt;";   break;
                case '"':  out += "&quot;"; break;
                case '\'': out += "&#39;";  break;
                default:   out += c;
            }
        }
        return out;
    }

    // Mask for sensitive tokens: shows only the last 4 characters
    static String maskSecret(const char* s) {
        size_t len = strlen(s);
        if (len == 0) return String();
        return String("****") + String(s + (len > 4 ? len - 4 : 0));
    }

    static String formatDuration(unsigned long ms) {
        unsigned long s = ms / 1000;
        char buf[24];
        snprintf(buf, sizeof(buf), "%luh %02lum %02lus", s / 3600, (s / 60) % 60, s % 60);
        return String(buf);
    }

    // Binary scale: 1024 B = 1 KB. One decimal place, and a .0 is dropped.
    // 1500 → "1.5 KB"; 1024 → "1 KB"; 1048576 → "1 MB".
    static String formatBytes(uint32_t n) {
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
        char buf[24];
        if (scale == 1) {
            snprintf(buf, sizeof(buf), "%lu B", (unsigned long)n);
            return String(buf);
        }
        unsigned long tenths =
            (unsigned long)(((unsigned long long)n * 10ULL + scale / 2) / scale);
        if (tenths % 10 == 0)
            snprintf(buf, sizeof(buf), "%lu %s", tenths / 10, unit);
        else
            snprintf(buf, sizeof(buf), "%lu.%lu %s", tenths / 10, tenths % 10, unit);
        return String(buf);
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
    uint8_t          _loginFails = 0;
    unsigned long    _loginWaitUntil = 0;
    uint8_t          _session = 0;   // 0 signed out, 1 authenticator setup, 2 signed in
    char             _sid[33] = {};
    unsigned long    _sidUntil = 0;
    uint8_t          _pending[Totp::KEY_LEN] = {};
    bool             _pendingSet = false;

    char _csrfToken[17]; // 16 hex + null
    char _csrfSid[17] = {};  // the session's form token; empty with no session

    static void _dispatch(void* self) { static_cast<WebService*>(self)->_route(); }

    // Route by path and method; anything else is a 404
    void _route() {
        const char* p = _server.path();
        if (_server.method() == HttpServer::GET) {
            if (!strcmp_P(p, PSTR("/")))           return _handleRoot();
            if (!strcmp_P(p, PSTR("/log")))        return _handleLog();
            if (!strcmp_P(p, PSTR("/status")))     return _handleStatus();
            if (!strcmp_P(p, PSTR("/cloudflare"))) return _handleCloudflare();
            if (!strcmp_P(p, PSTR("/ntfy")))       return _handleNtfy();
            if (!strcmp_P(p, PSTR("/mqtt")))       return _handleMqtt();
            if (!strcmp_P(p, PSTR("/password")))   return _handlePassword();
            if (!strcmp_P(p, PSTR("/login")))      return _handleLogin();
            if (!strcmp_P(p, PSTR("/mfa")))        return _handleMfa();
            if (!strcmp_P(p, PSTR("/iot")))        return _handleIot();
            if (!strcmp_P(p, PSTR("/config")))     return _handleConfig();
            if (!strcmp_P(p, PSTR("/iot/now")))    return _handleIotNow();
            // Local API: the same reading and commands, with a token instead of the login
            if (!strcmp_P(p, PSTR("/api/state")))  return _handleApiState();
            if (!strcmp_P(p, PSTR("/update")))     return _handleUpdate();
            // Embedded files (Assets.h, WebAssets.h): public, with no device data
            if (!strcmp_P(p, PSTR("/s.css")))
                return _asset("text/css", WEB_CSS_GZ, sizeof(WEB_CSS_GZ), true);
            if (!strcmp_P(p, PSTR("/iot.js")))
                return _asset("text/javascript", WEB_IOT_GZ, sizeof(WEB_IOT_GZ), true);
            if (!strcmp_P(p, PSTR("/i18n.js")))
                return _asset("text/javascript", WEB_I18N_GZ, sizeof(WEB_I18N_GZ), true);
            if (!strcmp_P(p, PSTR("/pinout.svg")))
                return _asset("image/svg+xml", ASSET_PINOUT_GZ, sizeof(ASSET_PINOUT_GZ), true);
            if (!strcmp_P(p, PSTR("/esp01.avif")))
                return _asset("image/avif", ASSET_PHOTO, sizeof(ASSET_PHOTO), false, false);
        } else if (_server.method() == HttpServer::POST) {
            if (!strcmp_P(p, PSTR("/save")))            return _handleSave();
            if (!strcmp_P(p, PSTR("/config/save")))     return _handleConfigSave();
            if (!strcmp_P(p, PSTR("/cloudflare/save"))) return _handleCloudflareSave();
            if (!strcmp_P(p, PSTR("/ntfy/save")))       return _handleNtfySave();
            if (!strcmp_P(p, PSTR("/mqtt/save")))       return _handleMqttSave();
            if (!strcmp_P(p, PSTR("/password/save")))   return _handlePasswordSave();
            if (!strcmp_P(p, PSTR("/login")))           return _handleLoginPost();
            if (!strcmp_P(p, PSTR("/logout")))          return _handleLogout();
            if (!strcmp_P(p, PSTR("/mfa")))             return _handleMfaPost();
            if (!strcmp_P(p, PSTR("/mfa/reset")))       return _handleMfaReset();
            if (!strcmp_P(p, PSTR("/iot/save")))        return _handleIotSave();
            if (!strcmp_P(p, PSTR("/iot/cmd")))         return _handleIotCmd();
            if (!strcmp_P(p, PSTR("/iot/token")))       return _handleIotToken();
            if (!strcmp_P(p, PSTR("/iot/clear")))       return _handleIotClear();
            if (!strcmp_P(p, PSTR("/api/cmd")))         return _handleApiCmd();
            if (!strcmp_P(p, PSTR("/update/auto")))     return _handleUpdateAuto();
            if (!strcmp_P(p, PSTR("/update/check")))    return _handleUpdateCheck();
            if (!strcmp_P(p, PSTR("/update/install")))  return _handleUpdateInstall();
        }
        _server.send(404, "text/plain", "");
    }

    // ── Security ──────────────────────────────────────────────────────────────

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

    bool _sessionOk() {
        if (_session == 0) return false;
        if ((long)(millis() - _sidUntil) >= 0) {
            _session = 0;
            _sid[0] = '\0';
            return false;
        }
        return _cookieMatches();
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
        bool p = _spanEq(pass.c_str(), pass.length(), _cfg.webpss.c_str(), _cfg.webpss.length(), 64);
        return u && p;
    }

    bool _mfaOn() const { return !Totp::blank(_cfg.totp); }

    // Time to check a code against: the device clock once SNTP has set it.
    // Until then (setup access point, no internet, a reboot without RAM
    // clock) the browser's, which i18n.js puts in t when the form is sent.
    // A time the browser makes up does not bring an old code back: every
    // step at or before the last one accepted (totpStep, kept in flash) is
    // refused, so it only opens a code nobody has seen, which would still
    // have to be guessed under the same wait. 0: no usable time.
    time_t _codeTime() const {
        time_t now = _iot.unixTime();
        if (now) return now;
        String t = _server.arg("t");
        const char* s = t.c_str();
        char* end;
        if (t.length() > 10 || *s < '0' || *s > '9') return 0;
        unsigned long v = strtoul(s, &end, 10);
        if (*end) return 0;
        return v > (unsigned long)Clock::MIN_VALID_EPOCH && v < 4000000000UL ? (time_t)v : 0;
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
    bool _confirmed() {
        time_t now = _codeTime();
        if (!now) {
            _errorPage(PSTR(L_CLOCK_NOT_SYNC), PSTR("/password"));
            return false;
        }
        uint32_t step = _cfg.totpStep;
        bool passOk = _credentialOk(_cfg.webusr, _server.arg("pass"));
        bool codeOk = _codeOk(_cfg.totp, now, step);
        if (!passOk || !codeOk) {
            _loginFailed(true);
            _errorPage(PSTR(L_SIGN_IN_FAILED), PSTR("/password"));
            return false;
        }
        _cfg.totpStep = step;
        return true;
    }

    void _ensurePending() {
        if (_pendingSet) return;
        Totp::randomKey(_pending);
        _pendingSet = true;
    }

    // Wrong password, code or token: the first mistakes pass; after that each
    // one doubles the wait (2 s up to 5 min), and nothing is tried meanwhile
    // (429). Getting it right resets the count. The count is per device, not
    // per address: there is no memory for a table, and from outside the home
    // the panel sits behind a tunnel with its own login (docs/REMOTE-ACCESS.md).
    bool _loginBlocked() {
        if (_loginFails < FREE_LOGIN_FAILS || (long)(millis() - _loginWaitUntil) >= 0) return false;
        _server.send(429, "text/plain", "Too many attempts; please wait");
        return true;
    }

    // attempt is false when the caller sent no credential (an API probe).
    void _loginFailed(bool attempt) {
        if (!attempt) return;
        if (_loginFails < 255) _loginFails++;
        if (_loginFails < FREE_LOGIN_FAILS) return;
        uint8_t doubling = _loginFails - FREE_LOGIN_FAILS;
        unsigned long wait = LOGIN_WAIT_MS << (doubling < 8 ? doubling : 8);
        _loginWaitUntil = millis() + (wait < LOGIN_WAIT_MAX_MS ? wait : LOGIN_WAIT_MAX_MS);
        if (_loginFails == FREE_LOGIN_FAILS) _log.log(PSTR("Login: repeated failures, growing wait"));
    }

    // The session's token for a browser holding the cookie, otherwise the
    // boot one. signIn: the sign-in form, which always carries the boot one.
    const char* _formToken(bool signIn = false) {
        return !signIn && _sessionOk() ? _csrfSid : _csrfToken;
    }

    bool _checkCSRF(bool signIn = false) {
        String got = _server.arg("_csrf");
        const char* want = _formToken(signIn);
        if (!*want || !_spanEq(got.c_str(), got.length(), want, strlen(want), sizeof(_csrfToken) - 1)) {
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
        _p(_HTML_HEAD_C);
        // Also while the authenticator is being set up: signing out there
        // drops a replacement and keeps the current key
        if (_sessionOk()) {
            _p(PSTR("<form class='out' action='/logout' method='POST'>"));
            _csrfField();
            _p(PSTR("<button type='submit'>" L_SIGN_OUT "</button></form>"));
        }
        _p(PSTR("</nav></header><main>"));
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
        if (_cfg.dev_name[0]) _s(escape(_cfg.dev_name));
        else _p(PSTR("ESP8266 DNS"));
    }

    // Footer and the translation; then the page's own scripts, if any
    void _pageFoot() {
        _p(_HTML_FOOT_A);
        _s(firmware_version);
        _p(_HTML_FOOT_B);
        _s(escape(_cfg.lang));
        _p(PSTR("'></script>"));
    }

    void _pageEnd() {
        _pageFoot();
        _p(PSTR("</body></html>"));
    }

    // A page with no known length ends when the connection closes
    void _p(PGM_P s) { _server.sendContent_P(s); }
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

    // Label + value on one line with a separator (the value is always escaped).
    // id, if any, lets the script update the value without reloading the page
    void _row(PGM_P label, const String& value, PGM_P id = nullptr) {
        _p(PSTR("<div class='r'><span>"));
        _p(label);
        if (id) {
            _p(PSTR("</span><b id='"));
            _p(id);
            _p(PSTR("'>"));
        } else {
            _p(PSTR("</span><b>"));
        }
        _s(escape(value));
        _p(PSTR("</b></div>"));
    }

    void _csrfField(bool signIn = false) {
        _p(PSTR("<input type='hidden' name='_csrf' value='"));
        _s(_formToken(signIn));
        _p(PSTR("'>"));
    }

    // Authenticator code, plus the browser's clock (t, filled in by i18n.js
    // when the form is sent; see _codeTime)
    void _codeInput() {
        _input(PSTR(L_AUTH_CODE), PSTR("code"), String(), _CODE_FIELD);
        _p(PSTR("<input type='hidden' name='t'>"));
    }

    void _alertOpen(PGM_P cls) {
        _p(PSTR("<div class='a "));
        _p(cls);
        _p(PSTR("'>"));
    }

    // Variable text: always escaped
    void _alert(PGM_P cls, const String& msg) {
        _alertOpen(cls);
        _s(escape(msg));
        _p(PSTR("</div>"));
    }

    // Fixed firmware text (PSTR, with no special HTML characters)
    void _alertP(PGM_P cls, PGM_P msg) {
        _alertOpen(cls);
        _p(msg);
        _p(PSTR("</div>"));
    }

    // The <label> wraps the field: implicit association for screen readers
    void _labelOpen(PGM_P label) {
        _p(PSTR("<label>"));
        _p(label);
    }

    void _input(PGM_P label, PGM_P name, const String& value, PGM_P extra) {
        _labelOpen(label);
        _p(PSTR("<input name='"));
        _p(name);
        _p(PSTR("' value='"));
        _s(escape(value));
        _p(PSTR("' "));
        _p(extra);
        _p(PSTR("></label>"));
    }

    void _passwordInput(PGM_P label, PGM_P name, PGM_P extra, PGM_P complete = nullptr) {
        _labelOpen(label);
        _p(PSTR("<input type='password' "));
        _p(complete ? complete : PSTR("autocomplete='new-password' "));
        _p(PSTR("name='"));
        _p(name);
        _p(PSTR("' "));
        _p(extra);
        _p(PSTR("></label>"));
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
        _alertP(PSTR("success"), msg);
        _p(PSTR("<meta http-equiv='refresh' content='2;url="));
        _p(backHref);
        _p(PSTR("'>"));
        _backButton(backHref);
        _pageEnd();
    }

    void _errorPage(PGM_P msg, PGM_P backHref) {
        _pageBegin(0, 400);
        _alertP(PSTR("danger"), msg);
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
        _cfg.webpss = pass1;
        return true;
    }

    void _otaProgressBar() {
        if (!_ota.inProgress()) return;
        String pct(_ota.progressPercent());
        _p(PSTR("<div class='a' role='status'>" L_OTA_PROGRESS " <b>"));
        _s(pct);
        _p(PSTR("%</b><progress max='100' value='"));
        _s(pct);
        _p(PSTR("'></progress><span class='m'>"));
        _s(formatBytes((uint32_t)_ota.bytesWritten()));
        _p(PSTR(" / "));
        _s(formatBytes((uint32_t)_ota.contentLength()));
        _p(PSTR("</span></div>"));
    }

    // ── Handlers ─────────────────────────────────────────────────────────────

    void _handleRoot() {
        if (!_guard()) return;
        _pageBegin();
        _deviceMark();

        if (_wifi.inConfigMode()) {
            _alertP(PSTR("warning"), PSTR(L_SETUP_MODE));
            _p(PSTR("<form action='/save' method='POST'>"));
            _csrfField();
            _input(PSTR("SSID"), PSTR("ssid"), _cfg.ssid, PSTR("required maxlength='32'"));
            _passwordInput(PSTR(L_WIFI_PASSWORD), PSTR("pass"), PSTR("maxlength='63'"));
            if (_cfg.webpss.length() == 0) {
                _p(PSTR("<h2>" L_PANEL_ACCESS "</h2>"));
                _input(PSTR(L_USER), PSTR("user"), _cfg.webusr.length() ? _cfg.webusr : String("admin"),
                       PSTR("required maxlength='32'"));
                _passwordInput(PSTR(L_PANEL_PASS_MIN), PSTR("pass1"), PSTR("required minlength='8'"));
                _passwordInput(PSTR(L_CONFIRM_PASS), PSTR("pass2"), PSTR("required minlength='8'"));
            }
            _button(PSTR(L_CONNECT));
        } else {
            _otaProgressBar();

            _box(PSTR(L_NETWORK));
            _row(PSTR("SSID"),       WiFi.SSID());
            _row(PSTR(L_LOCAL_IP),   WiFi.localIP().toString());
            _row(PSTR(L_PUBLIC_IP), _cfg.publicIP[0] ? String(_cfg.publicIP) : String("-"));
            _row(PSTR("RSSI"),       String(WiFi.RSSI()) + " dBm");
            _boxEnd();

            _box(PSTR("DNS Cloudflare"));
            _row(PSTR("Host"), _cfg.cf_host[0] ? String(_cfg.cf_host) : String("-"));
            _row(PSTR(L_LAST_UPDATE), _ago(_cfg.lastDnsUpdate != 0, _cfg.lastDnsUpdate));
            _boxEnd();
        }

        _pageEnd();
    }

    // Device settings: name, hostname, time zone and battery gauge
    void _handleConfig() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR(L_SETTINGS));
        _p(PSTR("<form action='/config/save' method='POST'>"));
        _csrfField();
        _input(PSTR(L_NAME " <span class='m'>" L_NAME_HINT "</span>"), PSTR("name"),
               _cfg.dev_name, PSTR("maxlength='31' placeholder='ESP8266 DNS'"));
        _input(PSTR("Hostname <span class='m'>" L_HOSTNAME_HINT "</span>"),
               PSTR("hostname"), _cfg.hostname,
               PSTR("maxlength='32' pattern='[A-Za-z0-9]([A-Za-z0-9\\-]*[A-Za-z0-9])?'"));
        _tzSelect();
        _pinSelect(PSTR(L_GAUGE " <span class='m'>" L_GAUGE_HINT "</span>"), PSTR("bat_sda"), _cfg.bat_sda,
                   PSTR(L_NONE));
        _selectOpen(PSTR(L_LANGUAGE), PSTR("lang"));
        _option("", PSTR(L_BROWSER_LANG), !_cfg.lang[0]);
        for (size_t i = 0; i < sizeof(_LANG_CODES) / sizeof(_LANG_CODES[0]); i++)
            _option(_LANG_CODES[i], _LANG_NAMES[i], strcmp(_cfg.lang, _LANG_CODES[i]) == 0);
        _selectEnd();
        _button(PSTR(L_SAVE));
        _pageEnd();
    }

    void _handleSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String ssid = _server.arg("ssid");
        String pass = _server.arg("pass");
        if (ssid.length() == 0 || ssid.length() > 32) {
            _errorPage(PSTR(L_INVALID_SSID), PSTR("/"));
            return;
        }
        if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
            _errorPage(PSTR(L_WIFI_PASS_LENGTH), PSTR("/"));
            return;
        }
        if (_cfg.webpss.length() == 0 && !_readPanelCredentials("/")) return;

        _cfg.ssid = ssid;
        _cfg.pass = pass;
        if (!_store.save()) {
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/"));
            return;
        }
        _savedPage(PSTR(L_WIFI_SET), PSTR("/"));
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
        int16_t tz = 0;
        String tzArg = _server.arg("tz");
        if (tzArg.length() && (!IoTService::parseI16(tzArg, -720, 840, tz) || !validTz(tz))) {
            _errorPage(PSTR(L_INVALID_TZ), PSTR("/config"));
            return;
        }
        String lang = _server.arg("lang");
        bool known = lang.length() == 0;
        for (size_t i = 0; !known && i < sizeof(_LANG_CODES) / sizeof(_LANG_CODES[0]); i++)
            known = lang == _LANG_CODES[i];
        if (!known) {
            _errorPage(PSTR(L_INVALID_LANG), PSTR("/config"));
            return;
        }
        int8_t bat = -1;
        String batArg = _server.arg("bat_sda");
        if (batArg.length() && (!IoTService::parsePin(batArg, bat) || !PowerService::pinsFree(bat, _cfg.iot))) {
            _errorPage(PSTR(L_GAUGE_PINS_TAKEN), PSTR("/config"));
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
            _errorPage(PSTR(L_INVALID_NAME), PSTR("/config"));
            return;
        }
        size_t n = host.length();
        ok = n < sizeof(_cfg.hostname) && (n == 0 || (host.c_str()[0] != '-' && host.c_str()[n - 1] != '-'));
        for (size_t i = 0; ok && i < n; i++) {
            char c = host.c_str()[i];
            ok = isalnum((unsigned char)c) || c == '-';
        }
        if (!ok) {
            _errorPage(PSTR(L_INVALID_HOSTNAME), PSTR("/config"));
            return;
        }
        strlcpy(_cfg.dev_name, name.c_str(), sizeof(_cfg.dev_name));
        strlcpy(_cfg.hostname, host.c_str(), sizeof(_cfg.hostname));
        _cfg.tz = tz;
        _cfg.bat_sda = bat;
        strlcpy(_cfg.lang, lang.c_str(), sizeof(_cfg.lang));
        if (!_store.save()) {
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/config"));
            return;
        }
        if (_cfg.hostname[0]) WiFi.hostname(_cfg.hostname);
        if (_mqtt) _mqtt->reconnect();   // discovery carries the new name
        if (_power) _power->begin();
        _savedPage(PSTR(L_SETTINGS_SAVED), PSTR("/config"));
    }

    void _handleLog() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR(L_SYSTEM_LOG));
        _p(PSTR("<pre>"));
        uint8_t cnt   = _log.count();
        uint8_t start = _log.startIndex();
        for (uint8_t i = 0; i < cnt; i++) {
            _s(escape(_log.entry(start + i)));
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

        _box(PSTR(L_SYSTEM));
        _row(PSTR(L_UPTIME),  formatDuration(millis()));
        _row(PSTR(L_LOCAL_TIME), _localTime());
        _row(PSTR(L_RESET),   ESP.getResetReason());
        _row(PSTR(L_CAUSE),   ESP.getResetInfo());
        _row(PSTR(L_CHIP_ID), String(ESP.getChipId(), HEX));
        _row(PSTR(L_AP_PASSWORD " " AP_SSID), _wifi.apPassword());
        _row(PSTR("CPU"),     String(ESP.getCpuFreqMHz()) + " MHz");
        _row(PSTR("SDK"),     String(ESP.getSdkVersion()));
        _boxEnd();

        _box(PSTR(L_MEMORY));
        _row(PSTR(L_FREE_HEAP),   formatBytes(ESP.getFreeHeap()));
        _row(PSTR(L_LARGEST_BLOCK),  formatBytes(ESP.getMaxFreeBlockSize()));
        _row(PSTR(L_FRAGMENTATION), String(ESP.getHeapFragmentation()) + "%");
        _boxEnd();

        _box(PSTR("Flash"));
        _row(PSTR("Total"),      formatBytes(ESP.getFlashChipSize()));
        _row(PSTR("Sketch"),     formatBytes(ESP.getSketchSize()));
        _row(PSTR(L_OTA_RESERVE), formatBytes(FlashLayout::OTA_RESERVE));
        _row(PSTR(L_FREE),      formatBytes(ESP.getFreeSketchSpace()));
        _boxEnd();

        _box(PSTR("LittleFS"));
        FSInfo fs;
        LittleFS.info(fs);
        _row(PSTR("Total"), formatBytes((uint32_t)fs.totalBytes));
        _row(PSTR(L_USED), formatBytes((uint32_t)fs.usedBytes));
        Dir dir = LittleFS.openDir("/");
        int fc = 0;
        while (dir.next()) fc++;
        _row(PSTR(L_FILES), String(fc));
        _boxEnd();

        _box(PSTR("Wi-Fi"));
        bool con = WiFi.isConnected();
        _row(PSTR("SSID"),    con ? WiFi.SSID()                 : String("-"));
        _row(PSTR("IP"),      con ? WiFi.localIP().toString()   : String("-"));
        _row(PSTR(L_GATEWAY), con ? WiFi.gatewayIP().toString() : String("-"));
        _row(PSTR(L_NETMASK), con ? WiFi.subnetMask().toString(): String("-"));
        _row(PSTR("DNS"),     con ? WiFi.dnsIP().toString()     : String("-"));
        _row(PSTR("MAC"),     WiFi.macAddress());
        _row(PSTR("RSSI"),    String(WiFi.RSSI()) + " dBm");
        _boxEnd();

        if (_power && _power->configured()) {
            _box(PSTR(L_BATTERY));
            bool ok = _power->source() != PowerService::UNKNOWN;
            _row(PSTR(L_STATE), PowerService::name(_power->source()));
            _row(PSTR(L_CHARGE),  ok ? String(_power->percent()) + "%" : String("-"));
            char b[16];
            snprintf(b, sizeof(b), "%u.%02u V", _power->millivolts() / 1000u,
                     (_power->millivolts() % 1000u) / 10u);
            _row(PSTR(L_VOLTAGE), ok ? String(b) : String("-"));
            int r = _power->rateTenths();
            snprintf(b, sizeof(b), "%s%d.%d %%/h", r < 0 ? "-" : "", abs(r) / 10, abs(r) % 10);
            _row(PSTR(L_RATE), ok ? String(b) : String("-"));
            _boxEnd();
        }

        // Checked every 5 min (PublicIPResolver); reverse name from the network's DNS
        _box(PSTR(L_PUBLIC_IP));
        bool ip = _cfg.publicIP[0];
        _row(PSTR("IP"),      ip ? String(_cfg.publicIP) : String("-"));
        _row(PSTR(L_REVERSE), _cfg.publicHost[0] ? String(_cfg.publicHost) : String("-"));
        _row(PSTR(L_CHANGED),   _ago(ip, _cfg.ipChanged));
        _row(PSTR(L_LAST_CHECK), _ago(ip, _cfg.ipChecked));
        _boxEnd();

        _pageEnd();
    }

    void _handleCloudflare() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("Cloudflare DDNS"));

        if (_cfg.cf_token[0])
            _alert(PSTR("success"), L_TOKEN_SET " " + maskSecret(_cfg.cf_token));
        else
            _alertP(PSTR("warning"), PSTR(L_TOKEN_NOT_SET));

        _p(PSTR("<form action='/cloudflare/save' method='POST'>"));
        _csrfField();

        // Token: an empty field keeps the current one; a filled one replaces it
        _passwordInput(PSTR(L_API_TOKEN " <span class='m'>" L_KEEP_BLANK "</span>"),
                       PSTR("token"), PSTR("placeholder='" L_NEW_TOKEN "' maxlength='47'"));
        _input(PSTR(L_ZONE_ID), PSTR("zone"), _cfg.cf_zone, PSTR("maxlength='39'"));
        _input(PSTR(L_HOSTNAME_DISPLAY), PSTR("host"), _cfg.cf_host, PSTR("maxlength='63'"));
        _input(PSTR(L_RECORD_ID), PSTR("record"), _cfg.cf_record, PSTR("maxlength='39'"));

        _button(PSTR(L_SAVE));
        _pageEnd();
    }

    void _handleCloudflareSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        // Checks everything in temporary buffers before changing the config
        char token[sizeof(_cfg.cf_token)];
        char zone[sizeof(_cfg.cf_zone)];
        char host[sizeof(_cfg.cf_host)];
        char record[sizeof(_cfg.cf_record)];

        if (!_argInto("token", token, sizeof(token), "/cloudflare")) return;
        if (!_argInto("zone",  zone,  sizeof(zone),  "/cloudflare")) return;
        if (!_argInto("host",  host,  sizeof(host),  "/cloudflare")) return;
        if (!_argInto("record", record, sizeof(record), "/cloudflare")) return;

        if (token[0]) strlcpy(_cfg.cf_token, token, sizeof(_cfg.cf_token));
        strlcpy(_cfg.cf_zone, zone, sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_host, host, sizeof(_cfg.cf_host));
        strlcpy(_cfg.cf_record, record, sizeof(_cfg.cf_record));

        if (!_store.save()) {
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/cloudflare"));
            return;
        }
        _savedPage(PSTR(L_CF_SET), PSTR("/cloudflare"));
    }

    void _handleNtfy() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR(L_NTFY_TITLE));

        if (_cfg.ntfy_topic[0])
            _alert(PSTR("success"), String(L_TOPIC " ") + _cfg.ntfy_topic);
        else
            _alertP(PSTR("warning"), PSTR(L_TOPIC_NOT_SET));
        if (_cfg.ntfy_cmd[0] && strlen(_cfg.ntfy_cmd) < MIN_CMD_TOPIC && !_cfg.ntfy_token[0])
            _alertP(PSTR("warning"), PSTR(L_SHORT_TOPIC_WARN));
        if (_ntfy) {
            _box(PSTR(L_NTFY_COMMANDS));
            _row(PSTR(L_STATE), _ntfy->commandStatus());
            _boxEnd();
        }

        _p(PSTR("<form action='/ntfy/save' method='POST'>"));
        _csrfField();
        _input(PSTR(L_NTFY_TOPIC), PSTR("topic"), _cfg.ntfy_topic,
               PSTR("maxlength='63' pattern='[A-Za-z0-9_\\-]*' placeholder='my-secret-topic'"));
        _p(PSTR("<p class='m'>" L_NTFY_TOPIC_HELP "</p>"));
        _input(PSTR(L_COMMAND_TOPIC), PSTR("cmd"), _cfg.ntfy_cmd,
               PSTR("id='cmd' maxlength='63' pattern='[A-Za-z0-9_\\-]*'"));
        _p(PSTR("<button type='button' class='b2' onclick=\"var a=new Uint8Array(12);"
                "crypto.getRandomValues(a);document.getElementById('cmd').value='cmd-'+"
                "Array.from(a,function(b){return('0'+b.toString(16)).slice(-2)}).join('')\">"
                L_GEN_RANDOM "</button><p class='m'>" L_CMD_TOPIC_HELP "</p>"));
        _passwordInput(PSTR(L_NTFY_TOKEN " <span class='m'>" L_OPTIONAL_KEEPS "</span>"), PSTR("token"),
                       PSTR("maxlength='47' placeholder='tk_...'"));
        _switch(PSTR("token_clear"), PSTR(L_DELETE_TOKEN), false);
        _button(PSTR(L_SAVE));
        _pageEnd();
    }

    void _handleNtfySave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String topic = _server.arg("topic");
        String cmd = _server.arg("cmd");
        String token = _server.arg("token");
        topic.trim();
        cmd.trim();
        token.trim();
        if (topic.length() > 0 && !NtfyNotifier::isValidTopic(topic)) {
            _errorPage(PSTR(L_INVALID_TOPIC), PSTR("/ntfy"));
            return;
        }
        if (cmd.length() > 0 && !NtfyNotifier::isValidTopic(cmd)) {
            _errorPage(PSTR(L_INVALID_CMD_TOPIC),
                       PSTR("/ntfy"));
            return;
        }
        // ntfy.sh topics are public by name: without a token, the command topic
        // must be long enough not to be guessed
        bool hasToken = token.length() > 0 || (_cfg.ntfy_token[0] && !_flag("token_clear"));
        if (cmd.length() > 0 && cmd.length() < MIN_CMD_TOPIC && !hasToken) {
            _errorPage(PSTR(L_SHORT_TOPIC),
                       PSTR("/ntfy"));
            return;
        }
        // The reply goes to the data topic: the same topic would read its own reply
        if (cmd.length() > 0 && cmd == topic) {
            _errorPage(PSTR(L_SAME_TOPIC), PSTR("/ntfy"));
            return;
        }
        bool tokenOk = token.length() < sizeof(_cfg.ntfy_token);
        for (size_t i = 0; tokenOk && i < token.length(); i++) {
            char c = token.c_str()[i];
            tokenOk = isalnum((unsigned char)c) || c == '_';
        }
        if (!tokenOk) {
            _errorPage(PSTR(L_INVALID_NTFY_TOKEN), PSTR("/ntfy"));
            return;
        }
        strlcpy(_cfg.ntfy_topic, topic.c_str(), sizeof(_cfg.ntfy_topic));
        strlcpy(_cfg.ntfy_cmd, cmd.c_str(), sizeof(_cfg.ntfy_cmd));
        if (_flag("token_clear")) _cfg.ntfy_token[0] = '\0';
        else if (token.length()) strlcpy(_cfg.ntfy_token, token.c_str(), sizeof(_cfg.ntfy_token));
        if (!_store.save()) {
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/ntfy"));
            return;
        }
        _savedPage(PSTR(L_NTFY_SAVED), PSTR("/ntfy"));
    }

    // ── MQTT ─────────────────────────────────────────────────────────────────

    void _handleMqtt() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("MQTT"));
        if (_mqtt) {
            _box(PSTR(L_CONNECTION));
            _row(PSTR(L_STATE), _mqtt->status());
            _row(PSTR(L_TOPICS), String(_mqtt->base()) + "/state, /cmd, /avail");
            _boxEnd();
        }
        _p(PSTR("<form action='/mqtt/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Broker <span class='m'>" L_BROKER_HINT "</span>"), PSTR("host"),
               _cfg.mqtt_host, PSTR("maxlength='63' placeholder='192.168.1.10:1883'"));
        _input(PSTR(L_USER), PSTR("user"), _cfg.mqtt_user,
               PSTR("maxlength='31' autocomplete='off'"));
        _passwordInput(PSTR(L_PASSWORD " <span class='m'>" L_BLANK_KEEPS "</span>"), PSTR("pass"),
                       PSTR("maxlength='63'"));
        _switch(PSTR("pass_clear"), PSTR(L_DELETE_PASSWORD), false);
        _p(PSTR("<p class='m'>" L_MQTT_HELP "</p>"));
        _button(PSTR(L_SAVE));
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
            _errorPage(PSTR(L_INVALID_BROKER), PSTR("/mqtt"));
            return;
        }
        if (user.length() >= sizeof(_cfg.mqtt_user) || pass.length() >= sizeof(_cfg.mqtt_pass)) {
            _errorPage(PSTR(L_USER_PASS_LONG), PSTR("/mqtt"));
            return;
        }
        strlcpy(_cfg.mqtt_host, host.c_str(), sizeof(_cfg.mqtt_host));
        strlcpy(_cfg.mqtt_user, user.c_str(), sizeof(_cfg.mqtt_user));
        if (_flag("pass_clear")) _cfg.mqtt_pass[0] = '\0';
        else if (pass.length()) strlcpy(_cfg.mqtt_pass, pass.c_str(), sizeof(_cfg.mqtt_pass));
        if (!_store.save()) {
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/mqtt"));
            return;
        }
        if (_mqtt) _mqtt->reconnect();
        _savedPage(PSTR(L_MQTT_SAVED), PSTR("/mqtt"));
    }

    void _handlePassword() {
        if (!_guard(true)) return;
        _pageBegin();
        _title(PSTR(L_PANEL_PASSWORD));
        if (_cfg.webpss.length() == 0)
            _alertP(PSTR("warning"), PSTR(L_SET_PASSWORD));
        _p(PSTR("<form action='/password/save' method='POST'>"));
        _csrfField();
        _input(PSTR(L_USER), PSTR("user"), _cfg.webusr.length() ? _cfg.webusr : String("admin"),
               PSTR("required maxlength='32'"));
        _passwordInput(PSTR(L_NEW_PASS_MIN), PSTR("pass1"), PSTR("required minlength='8' maxlength='64'"));
        _passwordInput(PSTR(L_CONFIRM_PASS), PSTR("pass2"), PSTR("required minlength='8' maxlength='64'"));
        if (_cfg.webpss.length()) {
            _passwordInput(PSTR(L_CURRENT_PASS), PSTR("pass"), PSTR("required maxlength='64'"),
                           PSTR("autocomplete='current-password' "));
            _codeInput();
        }
        _button(PSTR(L_SAVE));
        if (_cfg.webpss.length()) {
            _p(PSTR("<h2>" L_MFA_REPLACE "</h2><p class='m'>" L_MFA_REPLACE_HELP "</p>"));
            _p(PSTR("<form action='/mfa/reset' method='POST'>"));
            _csrfField();
            _passwordInput(PSTR(L_CURRENT_PASS), PSTR("pass"), PSTR("required maxlength='64'"),
                           PSTR("autocomplete='current-password' "));
            _codeInput();
            _button(PSTR(L_MFA_REPLACE));
        }
        _pageEnd();
    }

    // Changing a password that exists asks for it and for a code (_confirmed)
    void _handlePasswordSave() {
        if (!_guard(true)) return;
        if (!_checkCSRF()) return;
        if (_cfg.webpss.length() && !_confirmed()) return;
        if (!_readPanelCredentials("/password")) return;
        if (!_store.save()) {
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/password"));
            return;
        }
        _generateCSRF(); // rotates CSRF after a credentials change
        _clearSession(); // the old cookie must not survive a new password
        _savedPage(PSTR(L_PASSWORD_UPDATED), PSTR("/login"));
    }

    // ── Sign-in and authenticator ────────────────────────────────────────────

    void _loginPage(PGM_P err, bool warn) {
        _pageBegin(0, err && !warn ? 401 : 200);
        _title(PSTR(L_SIGN_IN));
        if (err) _alertP(warn ? PSTR("warning") : PSTR("danger"), err);
        else if (!_mfaOn()) _alertP(PSTR("warning"), PSTR(L_MFA_NEXT));
        _p(PSTR("<form action='/login' method='POST'>"));
        _csrfField(true);
        _input(PSTR(L_USER), PSTR("user"), _cfg.webusr,
               PSTR("required maxlength='32' autocomplete='username'"));
        _passwordInput(PSTR(L_PASSWORD), PSTR("pass"), PSTR("required maxlength='64'"),
                       PSTR("autocomplete='current-password' "));
        if (_mfaOn()) _codeInput();
        _button(PSTR(L_SIGN_IN));
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
        if (err) _alertP(warn ? PSTR("warning") : PSTR("danger"), err);
        _p(PSTR("<p class='m'>" L_MFA_HELP "</p>"));
        _showKey();
        _p(PSTR("<form action='/mfa' method='POST'>"));
        _csrfField();
        _codeInput();
        _button(PSTR(L_SAVE));
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
        time_t now = mfa ? _codeTime() : 0;
        // No time at all (no clock and no browser time): every attempt looks
        // the same, and it does not count. Otherwise a wrong password could
        // be told apart from a code that cannot be checked.
        if (mfa && !now) {
            _loginPage(PSTR(L_CLOCK_NOT_SYNC), true);
            return;
        }
        String user = _server.arg("user");
        user.trim();
        String pass = _server.arg("pass");
        bool passOk = _credentialOk(user, pass);
        uint32_t step = _cfg.totpStep;
        bool codeOk = !mfa || _codeOk(_cfg.totp, now, step);
        if (!passOk || !codeOk) {
            _loginFailed(true);
            _loginPage(PSTR(L_SIGN_IN_FAILED), false);
            return;
        }
        _loginFails = 0;
        if (mfa) {
            // The code is spent, also after a reboot. A failed save keeps it
            // in RAM until the next one.
            _cfg.totpStep = step;
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
        time_t now = _codeTime();
        if (!now) {
            _mfaPage(PSTR(L_CLOCK_NOT_SYNC), true);
            return;
        }
        uint32_t step = _cfg.totpStep;
        if (!_pendingSet || !_codeOk(_pending, now, step)) {
            _loginFailed(true);
            _mfaPage(PSTR(L_SIGN_IN_FAILED), false);
            return;
        }
        uint8_t prev[Totp::KEY_LEN];
        uint32_t prevStep = _cfg.totpStep;
        memcpy(prev, _cfg.totp, Totp::KEY_LEN);
        memcpy(_cfg.totp, _pending, Totp::KEY_LEN);
        _cfg.totpStep = step;
        if (!_store.save()) {
            memcpy(_cfg.totp, prev, Totp::KEY_LEN);
            _cfg.totpStep = prevStep;
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/mfa"));
            return;
        }
        _pendingSet = false;
        _startSession(2);
        _loginFails = 0;
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
        _loginFails = 0;
        _log.log(PSTR("Panel: authenticator reset"));
        _redirect("/mfa");
    }

    // Served straight from flash; the cache avoids resending on every page.
    // CSS, JS and the pinout carry the content hash in the URL (hashed): a
    // new version is another URL, so the browser keeps them for a year
    // without asking again.
    // The type is in RAM: it goes into the header's %s, which does not read
    // from flash. The photo is AVIF, already compressed: no gzip.
    void _asset(const char* type, const uint8_t* data, size_t len, bool hashed = false, bool gz = true) {
        _server.sendHeader("Cache-Control", hashed ? "max-age=31536000, immutable" : "max-age=604800");
        if (gz) _server.sendHeader("Content-Encoding", "gzip");
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
            _p(PSTR("<option value='"));
            _s(b);
            _p(tz == _cfg.tz ? PSTR("' selected>") : PSTR("'>"));
            snprintf(b, sizeof(b), "UTC%c%02d:%02d ", tz < 0 ? '-' : '+', a / 60, a % 60);
            _s(b);
            _p(place);
            _p(PSTR("</option>"));
            place += strlen_P(place) + 1;
        }
        _selectEnd();
    }

    // "HH:MM" from a minute of the day
    static String _hhmm(uint16_t m) {
        char b[8];
        snprintf(b, sizeof(b), "%02u:%02u", m / 60u, m % 60u);
        return String(b);
    }

    String _localTime() const {
        uint16_t m;
        return _iot.localMinute(m) ? _hhmm(m) : String(L_CLOCK_NOT_SYNC);
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
    static String _ago(bool happened, unsigned long at) {
        return happened ? L_AGO + formatDuration(millis() - at) : String(L_NEVER);
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
        _row(PSTR(L_AUTOMATIC_CAP), String(_cfg.ota_auto ? L_ON : L_OFF));
        if (_ota.availableVersion()[0] != '\0')
            _row(PSTR(L_AVAILABLE), _ota.availableVersion());
        if (_ota.detail()[0] != '\0')
            _row(PSTR(L_STATE), _ota.detail());
        _boxEnd();

        _p(PSTR("<form action='/update/auto' method='POST'>"));
        _csrfField();
        _switch(PSTR("ota_auto"), PSTR(L_AUTO_UPDATE), _cfg.ota_auto);
        _p(PSTR("<button class='b2'>" L_SAVE "</button></form>"));

        _p(PSTR("<form action='/update/check' method='POST'>"));
        _csrfField();
        _button(PSTR(L_CHECK_NOW));

        if (_ota.availableVersion()[0] != '\0' && !_ota.inProgress()) {
            _p(PSTR("<form action='/update/install' method='POST'>"));
            _csrfField();
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
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/update"));
            return;
        }
        _savedPage(_cfg.ota_auto
                       ? PSTR(L_AUTO_UPDATE_ON)
                       : PSTR(L_AUTO_UPDATE_OFF),
                   PSTR("/update"));
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
            _errorPage(PSTR(L_NO_NEW_VERSION), PSTR("/update"));
            return;
        }
        _ota.requestInstall();
        _go("/update");
    }

    // The module photo: AVIF with a transparent background (Assets.h)
    void _deviceMark() {
        _p(PSTR("<figure><img src='/esp01.avif' alt='ESP-01' width='220' height='209'>"
                "<figcaption class='m'>" L_FW_MODULE "<br>" L_PHOTO ": SparkFun, CC BY 2.0</figcaption></figure>"));
    }

    // ── IoT ──────────────────────────────────────────────────────────────────

    void _option(const char* value, PGM_P label, bool on) {
        _p(PSTR("<option value='"));
        _s(value);
        _p(on ? PSTR("' selected>") : PSTR("'>"));
        _p(label);
        _p(PSTR("</option>"));
    }

    void _selectOpen(PGM_P label, PGM_P name) {
        _labelOpen(label);
        _p(PSTR("<select name='"));
        _p(name);
        _p(PSTR("'>"));
    }

    void _selectEnd() { _p(PSTR("</select></label>")); }

    // Two 0/1 options (level, yes/no)
    void _pair(PGM_P label, PGM_P name, PGM_P zero, PGM_P one, bool value) {
        _selectOpen(label, name);
        _option("0", zero, !value);
        _option("1", one, value);
        _selectEnd();
    }

    // none: the text for -1 ("Choose" for IoT, "None" for the gauge)
    void _pinSelect(PGM_P label, PGM_P name, int8_t current, PGM_P none = nullptr) {
        if (!none) none = PSTR(L_CHOOSE);   // PSTR cannot be a default argument
        static const int8_t pins[] = {-1, 2, 0, 1, 3};
        static const char* const values[] = {"-1", "2", "0", "1", "3"};
        static const char* const labels[] = {
            nullptr, "GPIO2", "GPIO0", "TX (GPIO1)", "RX (GPIO3)"
        };
        _selectOpen(label, name);
        for (int i = 0; i < 5; i++) _option(values[i], i ? labels[i] : none, current == pins[i]);
        _selectEnd();
    }

    void _groupOpen(PGM_P label) {
        _p(PSTR("<optgroup label='"));
        _p(label);
        _p(PSTR("'>"));
    }

    void _sensorSelect(const char* current) {
        _selectOpen(PSTR(L_TYPE), PSTR("sensor"));
        for (uint8_t g = 0; g < 3; g++) {
            _groupOpen(iot::SENSOR_GROUPS[g]);
            for (const iot::SensorDef& s : iot::SENSORS)
                if (s.group == g) _option(s.slug, s.label, strcmp(s.slug, current) == 0);
            _p(PSTR("</optgroup>"));
        }
        _selectEnd();
    }

    void _actuatorSelect(const char* current) {
        _selectOpen(PSTR(L_TYPE), PSTR("actuator"));
        for (uint8_t cap = iot::C_ONOFF; cap <= iot::C_COLOR; cap++) {
            _groupOpen(iot::ACTUATOR_GROUPS[cap - 1]);
            for (const iot::ActuatorDef& a : iot::ACTUATORS)
                if (a.cap == cap) _option(a.slug, a.label, strcmp(a.slug, current) == 0);
            _p(PSTR("</optgroup>"));
        }
        _selectEnd();
    }

    // name goes into %s: it must be in RAM (snprintf_P only reads the format from flash).
    void _number(PGM_P label, const char* name, int value, int min, int max) {
        _labelOpen(label);
        char buf[112];
        snprintf_P(buf, sizeof(buf),
                   PSTR("<input type='number' name='%s' value='%d' min='%d' max='%d'></label>"),
                   name, value, min, max);
        _s(buf);
    }

    void _time(PGM_P label, PGM_P name, uint16_t minute) {
        _labelOpen(label);
        _p(PSTR("<input type='time' name='"));
        _p(name);
        _p(PSTR("' value='"));
        _s(_hhmm(minute));
        _p(PSTR("'></label>"));
    }

    void _fieldsetOpen(PGM_P id, PGM_P legend) {
        _p(PSTR("<fieldset id='"));
        _p(id);
        _p(PSTR("'><legend>"));
        _p(legend);
        _p(PSTR("</legend>"));
    }

    void _divOpen(PGM_P id) {
        _p(PSTR("<div id='"));
        _p(id);
        _p(PSTR("'>"));
    }

    void _radio(PGM_P value, PGM_P label, bool on) {
        _p(PSTR("<label class='sw'><input type='radio' name='role' value='"));
        _p(value);
        _p(on ? PSTR("' checked>") : PSTR("'>"));
        _p(label);
        _p(PSTR("</label>"));
    }

    // Range 0–N with the value beside it; lift the finger and the command goes
    void _range(PGM_P label, PGM_P id, int value, int max, PGM_P cmd, PGM_P unit) {
        char buf[40];   // "' min='0' max='180' value='180" = 30
        _labelOpen(label);
        _p(PSTR(": <output id='"));
        _p(id);
        _p(PSTR("o'>"));
        snprintf(buf, sizeof(buf), "%d", value);
        _s(buf);
        _p(PSTR("</output>"));
        _p(unit);
        _p(PSTR("<input type='range' id='"));
        _p(id);
        snprintf(buf, sizeof(buf), "' min='0' max='%d' value='%d", max, value);
        _s(buf);
        _p(PSTR("' oninput=\"$(this.id+'o').value=this.value\" onchange=\"c('"));
        _p(cmd);
        _p(PSTR("'+this.value)\"></label>"));
    }

    // Controls on the "Now" card, by what the actuator does
    void _controls() {
        const iot::ActuatorDef* a = _iot.actuatorDef();
        uint8_t cap = a->cap;
        char buf[32];
        _p(PSTR("<div class='ctl'>"));
        if (cap == iot::C_POSITION) {
            _p(PSTR("<button type='button' onclick=\"c('open')\">" L_OPEN_BTN "</button>"
                    "<button type='button' class='b2' onclick=\"c('close')\">" L_CLOSE_BTN "</button>"));
        } else {
            _p(PSTR("<button type='button' role='switch' id='sw' aria-checked='"));
            _p(_iot.actuatorOn() ? PSTR("true") : PSTR("false"));
            _p(PSTR("' onclick=\"c(this.getAttribute('aria-checked')=='true'?'off':'on')\">"));
            _p(a->label);
            _p(PSTR("</button>"));
        }
        _p(PSTR("<label>" L_PULSE_S "<input type='number' id='pd' min='1' max='3600' value='"));
        snprintf(buf, sizeof(buf), "%u", a->pulse);
        _s(buf);
        _p(PSTR("'></label><button type='button' class='b2' onclick=\"c('pulse '+$('pd').value)\">"
                L_PULSE "</button>"));
        if (_iot.hasRule()) {
            _p(PSTR("<button type='button' class='b2' id='au' onclick=\"c('auto')\""));
            if (!_iot.manual()) _p(PSTR(" hidden"));
            _p(PSTR(">" L_BACK_AUTO "</button>"));
        }
        _p(PSTR("</div>"));
        if (cap == iot::C_LEVEL || cap == iot::C_COLOR)
            _range(cap == iot::C_LEVEL ? PSTR(L_LEVEL) : PSTR(L_BRIGHTNESS),
                   PSTR("lv"), _iot.level(), 100, PSTR("level "), PSTR("%"));
        if (cap == iot::C_POSITION)
            _range(PSTR(L_ANGLE), PSTR("ag"), _iot.angle(), 180, PSTR("angle "), PSTR(" deg"));
        if (cap == iot::C_COLOR) {
            snprintf(buf, sizeof(buf), "%06lx", (unsigned long)_iot.color());
            _p(PSTR("<label>" L_COLOR "<input type='color' id='co' value='#"));
            _s(buf);
            _p(PSTR("' onchange=\"c('color '+this.value)\"></label>"));
        }
    }

    void _handleIot() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("IoT"));
        const IoTConfig& t = _cfg.iot;
        if (_iot.unknownType())
            _alertP(PSTR("warning"), PSTR(L_TYPE_UNKNOWN));
        if (t.notify && _cfg.ntfy_topic[0] == '\0')
            _alertP(PSTR("warning"), PSTR(L_SET_NTFY_TOPIC));

        bool live = _iot.hasSensor() || _iot.hasActuator();
        if (live) {
            _box(PSTR(L_NOW));
            if (_iot.hasSensor()) _row(PSTR(L_SENSOR), _iot.sensorText(), PSTR("ns"));
            if (_iot.hasActuator()) {
                _row(PSTR(L_ACTUATOR), _iot.actuatorText(), PSTR("na"));
                if (_iot.hasRule())
                    _row(PSTR(L_MODE), String(_iot.manual() ? L_MANUAL
                                              : _iot.inHours() ? L_AUTOMATIC
                                              : L_AUTO_OUTSIDE),
                         PSTR("nm"));
            }
            _boxEnd();
            if (_iot.hasActuator()) _controls();
            _p(PSTR("<p id='msg' class='m' role='status' aria-live='polite'></p>"));
        }

        // Drawing in English; i18n.js redraws it in the panel language
        _p(PSTR("<figure><img src='/pinout.svg?v=" ASSET_PINOUT_TAG "' width='1040' height='700' "
                "alt='" L_PINOUT_ALT "'></figure>"));

        bool hs = t.sensor[0] && t.sensorPin >= 0;
        bool ha = t.actuator[0] && t.actuatorPin >= 0;
        _p(PSTR("<form action='/iot/save' method='POST'>"));
        _csrfField();
        _fieldsetOpen(PSTR("fp"), PSTR(L_ROLE_QUESTION));
        _radio(PSTR("n"), PSTR(L_ROLE_NONE), !hs && !ha);
        _radio(PSTR("s"), PSTR(L_ROLE_SENSOR), hs && !ha);
        _radio(PSTR("a"), PSTR(L_ROLE_ACTUATOR), !hs && ha);
        _radio(PSTR("sa"), PSTR(L_ROLE_BOTH), hs && ha);
        _p(PSTR("</fieldset>"));

        _fieldsetOpen(PSTR("fs"), PSTR(L_SENSOR));
        _sensorSelect(t.sensor);
        _pinSelect(PSTR(L_PIN), PSTR("sensor_pin"), t.sensorPin);
        _divOpen(PSTR("sd"));
        _pair(PSTR(L_ACTIVE_WITH_PIN), PSTR("sensor_high"),
              PSTR(L_LOW), PSTR(L_HIGH), t.sensorHigh);
        _input(PSTR(L_ACTIVE_NAME), PSTR("sensor_on"), t.onText, PSTR("maxlength='15' placeholder='" L_TYPE_DEFAULT "'"));
        _input(PSTR(L_INACTIVE_NAME), PSTR("sensor_off"), t.offText,
               PSTR("maxlength='15' placeholder='" L_TYPE_DEFAULT "'"));
        _p(PSTR("</div>"));
        _divOpen(PSTR("sm"));
        _pair(PSTR(L_RULE_VALUE), PSTR("metric"),
              PSTR(L_HUMIDITY), PSTR(L_TEMPERATURE), t.metric == 1);
        _p(PSTR("</div><p class='m' id='sh'></p></fieldset>"));

        _fieldsetOpen(PSTR("fa"), PSTR(L_ACTUATOR));
        _actuatorSelect(t.actuator);
        _pinSelect(PSTR(L_PIN), PSTR("actuator_pin"), t.actuatorPin);
        _divOpen(PSTR("al"));
        _pair(PSTR(L_ON_WITH_PIN), PSTR("actuator_high"),
              PSTR(L_LOW_RELAY), PSTR(L_HIGH), t.actuatorHigh);
        _p(PSTR("</div>"));
        _divOpen(PSTR("ap"));
        _number(PSTR(L_LEVEL_ON), "duty_on", t.dutyOn, 0, 100);
        _number(PSTR(L_LEVEL_OFF), "duty_off", t.dutyOff, 0, 100);
        _p(PSTR("</div>"));
        _divOpen(PSTR("aa"));
        _number(PSTR(L_OPEN_ANGLE), "angle_on", t.angleOn, 0, 180);
        _number(PSTR(L_CLOSED_ANGLE), "angle_off", t.angleOff, 0, 180);
        _p(PSTR("</div>"));
        _divOpen(PSTR("ac"));
        _number(PSTR(L_LED_COUNT), "leds", t.leds, 1, IoTService::MAX_LEDS);
        _p(PSTR("</div>"));
        _number(PSTR(L_MAX_ON), "max_on", t.maxOn, 0, 1440);
        _pair(PSTR(L_POWER_UP), PSTR("restore"),
              PSTR(L_OFF_CAP), PSTR(L_LAST_COMMAND), t.restore);
        _p(PSTR("<p class='m' id='ah'></p></fieldset>"));

        _fieldsetOpen(PSTR("fr"), PSTR(L_RULE));
        _switch(PSTR("rule"), PSTR(L_RULE_SWITCH), t.rule);
        _divOpen(PSTR("rd"));
        _pair(PSTR(L_WHEN_ACTIVE), PSTR("toggle"),
              PSTR(L_ON_WHILE_ACTIVE), PSTR(L_FLIP), t.toggle);
        _p(PSTR("</div>"));
        _divOpen(PSTR("rn"));
        _pair(PSTR(L_ON_WHEN_VALUE), PSTR("above"),
              PSTR(L_BELOW_LIMIT), PSTR(L_ABOVE_LIMIT), t.above);
        _number(PSTR(L_LIMIT), "threshold", t.threshold, -999, 9999);
        _number(PSTR(L_MARGIN), "margin", t.margin, 0, 50);
        _divOpen(PSTR("rl"));
        _pair(PSTR(L_LEVEL), PSTR("linear"),
              PSTR(L_ONOFF_AT_LIMIT), PSTR(L_PROPORTIONAL), t.linear);
        _divOpen(PSTR("rh"));
        _number(PSTR(L_HIGH_LIMIT), "threshold_hi", t.thresholdHi, -999, 9999);
        _p(PSTR("</div>"));
        _p(PSTR("</div></div>"));
        _divOpen(PSTR("rt"));
        _switch(PSTR("window"), PSTR(L_WINDOW_SWITCH), t.window);
        _divOpen(PSTR("rw"));
        _time(PSTR(L_FROM), PSTR("win_start"), t.winStart);
        _time(PSTR(L_TO), PSTR("win_end"), t.winEnd);
        _p(PSTR("<p class='m'>" L_WINDOW_HELP "</p>"));
        _p(PSTR("</div></div><p class='m' id='rs'></p></fieldset>"));

        _switch(PSTR("notify"), PSTR(L_NOTIFY), t.notify);
        _button(PSTR(L_SAVE));
        if (live) _apiBox();
        if (hs || ha) {
            _p(PSTR("<form action='/iot/clear' method='POST'>"));
            _csrfField();
            _p(PSTR("<button class='b2'>" L_REMOVE_BOTH "</button></form>"));
        }

        _pageFoot();
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
        _p(PSTR("';</script><script src='/iot.js?v=" WEB_IOT_TAG "'></script></body></html>"));
    }

    // Reads an integer from the form within the range; otherwise answers 400
    bool _int(const char* name, int min, int max, int16_t& out) {
        if (IoTService::parseI16(_server.arg(name), min, max, out)) return true;
        _errorPage(PSTR(L_INVALID_VALUE), PSTR("/iot"));
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
            _errorPage(PSTR(L_INVALID_STATE_NAME), PSTR("/iot"));
            return false;
        }
        strlcpy(dst, v.c_str(), 16);
        return true;
    }

    bool _pinArg(const char* name, int8_t& out) {
        String v = _server.arg(name);   // a temporary inside && counts one more branch
        if (IoTService::parsePin(v, out) && iot::allowedPin(out)) return true;
        _errorPage(PSTR(L_CHOOSE_PIN), PSTR("/iot"));
        return false;
    }

    void _handleIotSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String role = _server.arg("role");
        bool hs = role == "s" || role == "sa";
        bool ha = role == "a" || role == "sa";
        if (!hs && !ha && role != "n") {
            _errorPage(PSTR(L_CHOOSE_ROLE), PSTR("/iot"));
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
                _errorPage(PSTR(L_INVALID_SENSOR), PSTR("/iot"));
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
        }

        if (ha) {
            String slug = _server.arg("actuator");
            int8_t i = iot::actuatorIndex(slug.c_str());
            if (i < 0) {
                _errorPage(PSTR(L_INVALID_ACTUATOR), PSTR("/iot"));
                return;
            }
            strlcpy(t.actuator, slug.c_str(), sizeof(t.actuator));
            cap = iot::ACTUATORS[i].cap;
            if (!_pinArg("actuator_pin", t.actuatorPin)) return;
            if (hs && (t.actuatorPin == t.sensorPin ||
                       (kind == iot::K_SHT3X && t.actuatorPin == I2cBus::sclFor(t.sensorPin)))) {
                _errorPage(PSTR(L_SAME_PINS), PSTR("/iot"));
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
            int16_t maxOn = 0;
            if (!_int("max_on", 0, 1440, maxOn)) return;
            t.maxOn = (uint16_t)maxOn;
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
                        _errorPage(PSTR(L_HIGH_ABOVE), PSTR("/iot"));
                        return;
                    }
                }
            }
            t.window = t.rule && _flag("window");
            if (t.window) {
                String from = _server.arg("win_start");   // a temporary inside || counts one more branch
                String to = _server.arg("win_end");
                if (!parseHhmm(from, t.winStart) || !parseHhmm(to, t.winEnd)) {
                    _errorPage(PSTR(L_INVALID_TIME), PSTR("/iot"));
                    return;
                }
                if (t.winStart == t.winEnd) {
                    _errorPage(PSTR(L_WINDOW_SAME), PSTR("/iot"));
                    return;
                }
            }
        }
        t.notify = _flag("notify");
        if (!PowerService::pinsFree(_cfg.bat_sda, t)) {
            _errorPage(PSTR(L_PIN_GAUGE), PSTR("/iot"));
            return;
        }
        if ((hs || ha) && !_cfg.api_token[0]) _newToken();

        _cfg.iot = t;
        if (!_store.save()) {
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/iot"));
            return;
        }
        _iot.begin();
        _savedPage(PSTR(L_IOT_SET), PSTR("/iot"));
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
        if (_loginBlocked()) return false;
        if (!_bearerOk()) {
            _loginFailed(_server.authorization().length() != 0);
            _server.sendHeader("WWW-Authenticate", "Bearer");
            _jsonError(401, "invalid token");
            return false;
        }
        _loginFails = 0;
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
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/iot"));
            return;
        }
        _savedPage(PSTR(L_NEW_TOKEN_DONE), PSTR("/iot"));
    }

    // Deletes sensor, actuator, rule and token. Asks for confirmation on its
    // own page: a plain form (no script of its own) that the screen reader announces.
    void _handleIotClear() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        if (!_flag("confirm")) {
            _pageBegin();
            _title(PSTR(L_REMOVE));
            _alertP(PSTR("warning"),
                    PSTR(L_REMOVE_WARNING));
            _p(PSTR("<form action='/iot/clear' method='POST'>"));
            _csrfField();
            _p(PSTR("<input type='hidden' name='confirm' value='1'>"
                    "<button class='bx'>" L_YES_REMOVE "</button></form>"));
            _backButton(PSTR("/iot"));
            _pageEnd();
            return;
        }
        _cfg.iot = IoTConfig();
        _cfg.api_token[0] = '\0';
        if (!_store.save()) {
            _errorPage(PSTR(L_SAVE_FAILED), PSTR("/iot"));
            return;
        }
        LittleFS.remove(IoTState::PATH);
        _iot.begin();
        _log.log(PSTR("IoT: sensor and actuator removed"));
        _savedPage(PSTR(L_REMOVED), PSTR("/iot"));
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
        _p(PSTR("<form action='/iot/token' method='POST'>"));
        _csrfField();
        _p(_cfg.api_token[0] ? PSTR("<button class='b2'>" L_GEN_NEW_TOKEN "</button></form>")
                             : PSTR("<button class='b2'>" L_GEN_TOKEN "</button></form>"));
    }
};
