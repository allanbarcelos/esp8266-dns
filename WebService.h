// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "HttpServer.h"
#include <LittleFS.h>
#include <base64.h>
#include "Config.h"
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

// ── PROGMEM ───────────────────────────────────────────────────────────────────
// Header split so a <meta refresh> can go before </head>. The CSS
// (web/panel.css) comes from the device itself, gzipped and cached: in AP
// mode there is no internet to fetch a framework.

static const char _HTML_HEAD_A[] PROGMEM =
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>";

static const char _HTML_HEAD_B[] PROGMEM =
    "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"
    "</head><body><header><b>&#9679; ";

static const char _HTML_HEAD_C[] PROGMEM =
    "<nav aria-label='Menu'><a href='/'>Home</a><a href='/status'>Status</a>"
    "<a href='/log'>Logs</a><a href='/cloudflare'>Cloudflare</a><a href='/ntfy'>Ntfy</a><a href='/mqtt'>MQTT</a>"
    "<a href='/iot'>IoT</a><a href='/config'>Settings</a><a href='/update'>Update</a>"
    "<a href='/password'>Password</a>"
    "</nav></header><main>";

static const char _HTML_FOOT_A[] PROGMEM = "</main><footer>Firmware: ";

static const char _HTML_FOOT_B[] PROGMEM = "</footer></body></html>";

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

    const char* csrfToken() const { return _csrfToken; }

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

    char _csrfToken[17]; // 16 hex + null

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
            if (!strcmp_P(p, PSTR("/iot")))        return _handleIot();
            if (!strcmp_P(p, PSTR("/config")))     return _handleConfig();
            if (!strcmp_P(p, PSTR("/iot/now")))    return _handleIotNow();
            // Local API: the same reading and commands, with a token instead of the login
            if (!strcmp_P(p, PSTR("/api/state")))  return _handleApiState();
            if (!strcmp_P(p, PSTR("/update")))     return _handleUpdate();
            // Embedded images (Assets.h): public, with no device data
            if (!strcmp_P(p, PSTR("/esp01.webp")))
                return _asset("image/webp", ASSET_PHOTO, sizeof(ASSET_PHOTO), false);
            if (!strcmp_P(p, PSTR("/s.css")))
                return _asset("text/css", WEB_CSS_GZ, sizeof(WEB_CSS_GZ), true);
            if (!strcmp_P(p, PSTR("/iot.js")))
                return _asset("text/javascript", WEB_IOT_GZ, sizeof(WEB_IOT_GZ), true);
            if (!strcmp_P(p, PSTR("/pinout.svg")))
                return _asset("image/svg+xml", ASSET_PINOUT_GZ, sizeof(ASSET_PINOUT_GZ), true);
        } else if (_server.method() == HttpServer::POST) {
            if (!strcmp_P(p, PSTR("/save")))            return _handleSave();
            if (!strcmp_P(p, PSTR("/config/save")))     return _handleConfigSave();
            if (!strcmp_P(p, PSTR("/cloudflare/save"))) return _handleCloudflareSave();
            if (!strcmp_P(p, PSTR("/ntfy/save")))       return _handleNtfySave();
            if (!strcmp_P(p, PSTR("/mqtt/save")))       return _handleMqttSave();
            if (!strcmp_P(p, PSTR("/password/save")))   return _handlePasswordSave();
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

    // Token from the hardware RNG on every boot and every password change
    void _generateCSRF() {
        snprintf(_csrfToken, sizeof(_csrfToken), "%08lx%08lx",
                 (unsigned long)ESP.random(), (unsigned long)ESP.random());
    }

    // Authentication plus a required password.
    // With no password set, the panel only allows AP mode (first setup) and
    // the password page (allowSetup).
    bool _guard(bool allowSetup = false) {
        if (_cfg.webpss.length() == 0) {
            if (allowSetup || _wifi.inConfigMode()) return true;
            _server.sendHeader("Location", "/password");
            _server.send(302, "text/plain", "");
            return false;
        }
        // cppcheck-suppress knownConditionTrueFalse ; the counter changes between requests
        if (_loginBlocked()) return false;
        // Basic only: the browser would use Digest only if the device asked for it
        if (!_authIs("Basic ", base64::encode(_cfg.webusr + ':' + _cfg.webpss).c_str())) {
            _loginFailed();
            _server.sendHeader("WWW-Authenticate", "Basic realm=\"ESP8266\"");
            _server.send(401, "text/plain", "");
            return false;
        }
        _loginFails = 0;
        return true;
    }

    // Wrong password or token: the first mistakes pass; after that each one
    // doubles the wait (2 s up to 5 min), and nothing is tried meanwhile
    // (429). Getting it right resets the count. The count is per device, not
    // per address: there is no memory for a table, and from outside the home
    // the panel sits behind a tunnel with its own login (docs/ACESSO-REMOTO.md).
    bool _loginBlocked() {
        if (_loginFails < FREE_LOGIN_FAILS || (long)(millis() - _loginWaitUntil) >= 0) return false;
        _server.send(429, "text/plain", "Too many attempts; please wait");
        return true;
    }

    void _loginFailed() {
        // The browser's first request comes without credentials: not a mistake
        if (_server.authorization().length() == 0) return;
        if (_loginFails < 255) _loginFails++;
        if (_loginFails < FREE_LOGIN_FAILS) return;
        uint8_t doubling = _loginFails - FREE_LOGIN_FAILS;
        unsigned long wait = LOGIN_WAIT_MS << (doubling < 8 ? doubling : 8);
        _loginWaitUntil = millis() + (wait < LOGIN_WAIT_MAX_MS ? wait : LOGIN_WAIT_MAX_MS);
        if (_loginFails == FREE_LOGIN_FAILS) _log.log(PSTR("Login: repeated failures, growing wait"));
    }

    bool _checkCSRF() {
        if (_server.arg("_csrf") != _csrfToken) {
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
    }

    // Top right corner: on supply, on battery (%) or charging
    void _powerMark() {
        if (!_power) return;
        PowerService::Source s = _power->source();
        _p(s == PowerService::BATTERY && _power->percent() < 20 ? PSTR("<span class='pw lo'")
                                                                 : PSTR("<span class='pw'"));
        if (s == PowerService::MAINS) {
            _p(PSTR(" title='On supply'>&#9889; Power</span>"));
            return;
        }
        if (s == PowerService::UNKNOWN) {
            _p(PSTR(" title='Gauge not answering'>&#128267; Battery ?</span>"));
            return;
        }
        _p(s == PowerService::CHARGING ? PSTR(" title='Battery charging'>&#9728; Charging ")
                                       : PSTR(" title='On battery'>&#128267; Battery "));
        _s(String(_power->percent()));
        _p(PSTR("%</span>"));
    }

    // Device name in the title and at the top; without one, the project's
    void _name() {
        if (_cfg.dev_name[0]) _s(escape(_cfg.dev_name));
        else _p(PSTR("ESP8266 DNS"));
    }

    void _pageEnd() {
        _p(_HTML_FOOT_A);
        _s(firmware_version);
        _p(_HTML_FOOT_B);
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

    void _csrfField() {
        _p(PSTR("<input type='hidden' name='_csrf' value='"));
        _s(_csrfToken);
        _p(PSTR("'>"));
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

    void _passwordInput(PGM_P label, PGM_P name, PGM_P extra) {
        _labelOpen(label);
        _p(PSTR("<input type='password' autocomplete='new-password' name='"));
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
        _p(PSTR("'>Back</a>"));
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
            _errorPage(PSTR("Value too long"), backHref);
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
            _errorPage(PSTR("Invalid user"), backHref);
            return false;
        }
        if (pass1 != _server.arg("pass2")) {
            _errorPage(PSTR("The passwords do not match"), backHref);
            return false;
        }
        if (pass1.length() < MIN_PANEL_PASSWORD_LEN || pass1.length() > 64) {
            _errorPage(PSTR("The password must have 8 to 64 characters"), backHref);
            return false;
        }
        _cfg.webusr = user;
        _cfg.webpss = pass1;
        return true;
    }

    void _otaProgressBar() {
        if (!_ota.inProgress()) return;
        String pct(_ota.progressPercent());
        _p(PSTR("<div class='a' role='status'>OTA in progress: <b>"));
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
            _alertP(PSTR("warning"), PSTR("Setup mode - connect to Wi-Fi"));
            _p(PSTR("<form action='/save' method='POST'>"));
            _csrfField();
            _input(PSTR("SSID"), PSTR("ssid"), _cfg.ssid, PSTR("required maxlength='32'"));
            _passwordInput(PSTR("Wi-Fi password"), PSTR("pass"), PSTR("maxlength='63'"));
            if (_cfg.webpss.length() == 0) {
                _p(PSTR("<h2>Panel access</h2>"));
                _input(PSTR("User"), PSTR("user"), _cfg.webusr.length() ? _cfg.webusr : String("admin"),
                       PSTR("required maxlength='32'"));
                _passwordInput(PSTR("Panel password (min. 8)"), PSTR("pass1"), PSTR("required minlength='8'"));
                _passwordInput(PSTR("Confirm password"), PSTR("pass2"), PSTR("required minlength='8'"));
            }
            _button(PSTR("Connect"));
        } else {
            _otaProgressBar();

            _box(PSTR("Network"));
            _row(PSTR("SSID"),       WiFi.SSID());
            _row(PSTR("Local IP"),   WiFi.localIP().toString());
            _row(PSTR("Public IP"), _cfg.publicIP[0] ? String(_cfg.publicIP) : String("-"));
            _row(PSTR("RSSI"),       String(WiFi.RSSI()) + " dBm");
            _boxEnd();

            _box(PSTR("DNS Cloudflare"));
            _row(PSTR("Host"), _cfg.cf_host[0] ? String(_cfg.cf_host) : String("-"));
            _row(PSTR("Last update"), _ago(_cfg.lastDnsUpdate != 0, _cfg.lastDnsUpdate));
            _boxEnd();
        }

        _pageEnd();
    }

    // Device settings: name, hostname, time zone and battery gauge
    void _handleConfig() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("Settings"));
        _p(PSTR("<form action='/config/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Name <span class='m'>(panel and Home Assistant)</span>"), PSTR("name"),
               _cfg.dev_name, PSTR("maxlength='31' placeholder='ESP8266 DNS'"));
        _input(PSTR("Hostname <span class='m'>(router; applies on the next connection)</span>"),
               PSTR("hostname"), _cfg.hostname,
               PSTR("maxlength='32' pattern='[A-Za-z0-9]([A-Za-z0-9\\-]*[A-Za-z0-9])?'"));
        _tzSelect();
        _pinSelect(PSTR("MAX17048 battery gauge <span class='m'>(SDA; SCL goes on the pair: "
                        "GPIO0 with GPIO2, RX with TX)</span>"), PSTR("bat_sda"), _cfg.bat_sda,
                   PSTR("None"));
        _button(PSTR("Save"));
        _pageEnd();
    }

    void _handleSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String ssid = _server.arg("ssid");
        String pass = _server.arg("pass");
        if (ssid.length() == 0 || ssid.length() > 32) {
            _errorPage(PSTR("Invalid SSID"), PSTR("/"));
            return;
        }
        if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
            _errorPage(PSTR("The Wi-Fi password must have 8 to 63 characters"), PSTR("/"));
            return;
        }
        if (_cfg.webpss.length() == 0 && !_readPanelCredentials("/")) return;

        _cfg.ssid = ssid;
        _cfg.pass = pass;
        if (!_store.save()) {
            _errorPage(PSTR("Could not save the configuration"), PSTR("/"));
            return;
        }
        _savedPage(PSTR("Wi-Fi set! Restarting..."), PSTR("/"));
        _wifi.scheduleRestart(2000);
    }

    // Name: up to 31 bytes, with no quotes, backslash or control characters
    // (it goes unescaped into the discovery JSON). Hostname: letters, digits
    // and hyphen, with no hyphen at either end (RFC 1123).
    void _handleConfigSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        String name = _server.arg("name");
        String host = _server.arg("hostname");
        int16_t tz = 0;
        String tzArg = _server.arg("tz");
        if (tzArg.length() && (!IoTService::parseI16(tzArg, -720, 840, tz) || !validTz(tz))) {
            _errorPage(PSTR("Invalid time zone"), PSTR("/config"));
            return;
        }
        int8_t bat = -1;
        String batArg = _server.arg("bat_sda");
        if (batArg.length() && (!IoTService::parsePin(batArg, bat) || !PowerService::pinsFree(bat, _cfg.iot))) {
            _errorPage(PSTR("The gauge pins are taken by the sensor or the actuator"), PSTR("/config"));
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
            _errorPage(PSTR("Invalid name (up to 31 characters, no quotes)"), PSTR("/config"));
            return;
        }
        size_t n = host.length();
        ok = n < sizeof(_cfg.hostname) && (n == 0 || (host.c_str()[0] != '-' && host.c_str()[n - 1] != '-'));
        for (size_t i = 0; ok && i < n; i++) {
            char c = host.c_str()[i];
            ok = isalnum((unsigned char)c) || c == '-';
        }
        if (!ok) {
            _errorPage(PSTR("Invalid hostname (letters, digits and hyphen)"), PSTR("/config"));
            return;
        }
        strlcpy(_cfg.dev_name, name.c_str(), sizeof(_cfg.dev_name));
        strlcpy(_cfg.hostname, host.c_str(), sizeof(_cfg.hostname));
        _cfg.tz = tz;
        _cfg.bat_sda = bat;
        if (!_store.save()) {
            _errorPage(PSTR("Could not save the configuration"), PSTR("/config"));
            return;
        }
        if (_cfg.hostname[0]) WiFi.hostname(_cfg.hostname);
        if (_mqtt) _mqtt->reconnect();   // discovery carries the new name
        if (_power) _power->begin();
        _savedPage(PSTR("Settings saved!"), PSTR("/config"));
    }

    void _handleLog() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("System log"));
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
        _title(PSTR("Status"));

        _otaProgressBar();

        _box(PSTR("System"));
        _row(PSTR("Uptime"),  formatDuration(millis()));
        _row(PSTR("Local time"), _localTime());
        _row(PSTR("Reset"),   ESP.getResetReason());
        _row(PSTR("Cause"),   ESP.getResetInfo());
        _row(PSTR("Chip ID"), String(ESP.getChipId(), HEX));
        _row(PSTR("AP password " AP_SSID), _wifi.apPassword());
        _row(PSTR("CPU"),     String(ESP.getCpuFreqMHz()) + " MHz");
        _row(PSTR("SDK"),     String(ESP.getSdkVersion()));
        _boxEnd();

        _box(PSTR("Memory"));
        _row(PSTR("Free heap"),   formatBytes(ESP.getFreeHeap()));
        _row(PSTR("Largest block"),  formatBytes(ESP.getMaxFreeBlockSize()));
        _row(PSTR("Fragmentation"), String(ESP.getHeapFragmentation()) + "%");
        _boxEnd();

        _box(PSTR("Flash"));
        _row(PSTR("Total"),      formatBytes(ESP.getFlashChipSize()));
        _row(PSTR("Sketch"),     formatBytes(ESP.getSketchSize()));
        _row(PSTR("OTA reserve"), formatBytes(FlashLayout::OTA_RESERVE));
        _row(PSTR("Free"),      formatBytes(ESP.getFreeSketchSpace()));
        _boxEnd();

        _box(PSTR("LittleFS"));
        FSInfo fs;
        LittleFS.info(fs);
        _row(PSTR("Total"), formatBytes((uint32_t)fs.totalBytes));
        _row(PSTR("Used"), formatBytes((uint32_t)fs.usedBytes));
        Dir dir = LittleFS.openDir("/");
        int fc = 0;
        while (dir.next()) fc++;
        _row(PSTR("Files"), String(fc));
        _boxEnd();

        _box(PSTR("Wi-Fi"));
        bool con = WiFi.isConnected();
        _row(PSTR("SSID"),    con ? WiFi.SSID()                 : String("-"));
        _row(PSTR("IP"),      con ? WiFi.localIP().toString()   : String("-"));
        _row(PSTR("Gateway"), con ? WiFi.gatewayIP().toString() : String("-"));
        _row(PSTR("Netmask"), con ? WiFi.subnetMask().toString(): String("-"));
        _row(PSTR("DNS"),     con ? WiFi.dnsIP().toString()     : String("-"));
        _row(PSTR("MAC"),     WiFi.macAddress());
        _row(PSTR("RSSI"),    String(WiFi.RSSI()) + " dBm");
        _boxEnd();

        if (_power && _power->configured()) {
            _box(PSTR("Battery"));
            bool ok = _power->source() != PowerService::UNKNOWN;
            _row(PSTR("State"), PowerService::name(_power->source()));
            _row(PSTR("Charge"),  ok ? String(_power->percent()) + "%" : String("-"));
            char b[16];
            snprintf(b, sizeof(b), "%u.%02u V", _power->millivolts() / 1000u,
                     (_power->millivolts() % 1000u) / 10u);
            _row(PSTR("Voltage"), ok ? String(b) : String("-"));
            int r = _power->rateTenths();
            snprintf(b, sizeof(b), "%s%d.%d %%/h", r < 0 ? "-" : "", abs(r) / 10, abs(r) % 10);
            _row(PSTR("Rate"), ok ? String(b) : String("-"));
            _boxEnd();
        }

        // Checked every 5 min (PublicIPResolver); reverse name from the network's DNS
        _box(PSTR("Public IP"));
        bool ip = _cfg.publicIP[0];
        _row(PSTR("IP"),      ip ? String(_cfg.publicIP) : String("-"));
        _row(PSTR("Reverse"), _cfg.publicHost[0] ? String(_cfg.publicHost) : String("-"));
        _row(PSTR("Changed"),   _ago(ip, _cfg.ipChanged));
        _row(PSTR("Last check"), _ago(ip, _cfg.ipChecked));
        _boxEnd();

        _pageEnd();
    }

    void _handleCloudflare() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("Cloudflare DDNS"));

        if (_cfg.cf_token[0])
            _alert(PSTR("success"), "Token set: " + maskSecret(_cfg.cf_token));
        else
            _alertP(PSTR("warning"), PSTR("Token not set"));

        _p(PSTR("<form action='/cloudflare/save' method='POST'>"));
        _csrfField();

        // Token: an empty field keeps the current one; a filled one replaces it
        _passwordInput(PSTR("API Token <span class='m'>(leave blank to keep)</span>"),
                       PSTR("token"), PSTR("placeholder='New token' maxlength='47'"));
        _input(PSTR("Zone ID"), PSTR("zone"), _cfg.cf_zone, PSTR("maxlength='39'"));
        _input(PSTR("Hostname (display)"), PSTR("host"), _cfg.cf_host, PSTR("maxlength='63'"));
        _input(PSTR("Record ID"), PSTR("record"), _cfg.cf_record, PSTR("maxlength='39'"));

        _button(PSTR("Save"));
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
            _errorPage(PSTR("Could not save the configuration"), PSTR("/cloudflare"));
            return;
        }
        _savedPage(PSTR("Cloudflare set!"), PSTR("/cloudflare"));
    }

    void _handleNtfy() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("ntfy notifications"));

        if (_cfg.ntfy_topic[0])
            _alert(PSTR("success"), String("Topic: ") + _cfg.ntfy_topic);
        else
            _alertP(PSTR("warning"), PSTR("Topic not set - notifications off"));
        if (_cfg.ntfy_cmd[0] && strlen(_cfg.ntfy_cmd) < MIN_CMD_TOPIC && !_cfg.ntfy_token[0])
            _alertP(PSTR("warning"), PSTR("Short command topic and no token: anyone who guesses "
                                          "the name controls the device"));
        if (_ntfy) {
            _box(PSTR("Commands over ntfy"));
            _row(PSTR("State"), _ntfy->commandStatus());
            _boxEnd();
        }

        _p(PSTR("<form action='/ntfy/save' method='POST'>"));
        _csrfField();
        _input(PSTR("ntfy.sh topic"), PSTR("topic"), _cfg.ntfy_topic,
               PSTR("maxlength='63' pattern='[A-Za-z0-9_\\-]*' placeholder='my-secret-topic'"));
        _p(PSTR("<p class='m'>Gets the IP at boot and when it changes, and the IoT readings "
                "and changes. Leave blank to turn it off.</p>"));
        _input(PSTR("Command topic"), PSTR("cmd"), _cfg.ntfy_cmd,
               PSTR("id='cmd' maxlength='63' pattern='[A-Za-z0-9_\\-]*'"));
        _p(PSTR("<button type='button' class='b2' onclick=\"var a=new Uint8Array(12);"
                "crypto.getRandomValues(a);document.getElementById('cmd').value='cmd-'+"
                "Array.from(a,function(b){return('0'+b.toString(16)).slice(-2)}).join('')\">"
                "Generate random name</button>"
                "<p class='m'>Each message published to this topic becomes an actuator command "
                "(on, off, pulse 5, 50%, open, auto). Anyone who knows the name can "
                "control it: use a long random name, different from the topic above, or an "
                "access token. Blank turns it off.</p>"));
        _passwordInput(PSTR("ntfy access token <span class='m'>(optional; blank keeps it)"
                            "</span>"), PSTR("token"), PSTR("maxlength='47' placeholder='tk_...'"));
        _switch(PSTR("token_clear"), PSTR("Delete the saved token"), false);
        _button(PSTR("Save"));
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
            _errorPage(PSTR("Invalid topic (use letters, digits, _ or -)"), PSTR("/ntfy"));
            return;
        }
        if (cmd.length() > 0 && !NtfyNotifier::isValidTopic(cmd)) {
            _errorPage(PSTR("Invalid command topic (use letters, digits, _ or -)"),
                       PSTR("/ntfy"));
            return;
        }
        // ntfy.sh topics are public by name: without a token, the command topic
        // must be long enough not to be guessed
        bool hasToken = token.length() > 0 || (_cfg.ntfy_token[0] && !_flag("token_clear"));
        if (cmd.length() > 0 && cmd.length() < MIN_CMD_TOPIC && !hasToken) {
            _errorPage(PSTR("Short command topic: use Generate random name or a token"),
                       PSTR("/ntfy"));
            return;
        }
        // The reply goes to the data topic: the same topic would read its own reply
        if (cmd.length() > 0 && cmd == topic) {
            _errorPage(PSTR("The command topic must be different"), PSTR("/ntfy"));
            return;
        }
        bool tokenOk = token.length() < sizeof(_cfg.ntfy_token);
        for (size_t i = 0; tokenOk && i < token.length(); i++) {
            char c = token.c_str()[i];
            tokenOk = isalnum((unsigned char)c) || c == '_';
        }
        if (!tokenOk) {
            _errorPage(PSTR("Invalid ntfy token"), PSTR("/ntfy"));
            return;
        }
        strlcpy(_cfg.ntfy_topic, topic.c_str(), sizeof(_cfg.ntfy_topic));
        strlcpy(_cfg.ntfy_cmd, cmd.c_str(), sizeof(_cfg.ntfy_cmd));
        if (_flag("token_clear")) _cfg.ntfy_token[0] = '\0';
        else if (token.length()) strlcpy(_cfg.ntfy_token, token.c_str(), sizeof(_cfg.ntfy_token));
        if (!_store.save()) {
            _errorPage(PSTR("Could not save the configuration"), PSTR("/ntfy"));
            return;
        }
        _savedPage(PSTR("ntfy settings saved!"), PSTR("/ntfy"));
    }

    // ── MQTT ─────────────────────────────────────────────────────────────────

    void _handleMqtt() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("MQTT"));
        if (_mqtt) {
            _box(PSTR("Connection"));
            _row(PSTR("State"), _mqtt->status());
            _row(PSTR("Topics"), String(_mqtt->base()) + "/state, /cmd, /avail");
            _boxEnd();
        }
        _p(PSTR("<form action='/mqtt/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Broker <span class='m'>(host or host:port)</span>"), PSTR("host"),
               _cfg.mqtt_host, PSTR("maxlength='63' placeholder='192.168.1.10:1883'"));
        _input(PSTR("User"), PSTR("user"), _cfg.mqtt_user,
               PSTR("maxlength='31' autocomplete='off'"));
        _passwordInput(PSTR("Password <span class='m'>(blank keeps it)</span>"), PSTR("pass"),
                       PSTR("maxlength='63'"));
        _switch(PSTR("pass_clear"), PSTR("Delete the saved password"), false);
        _p(PSTR("<p class='m'>Home Assistant finds the device on its own (discovery). "
                "It publishes the state (the same JSON as the API) and takes commands (the "
                "same text as the panel). No TLS: use a broker on the local network, with a "
                "user and password; anyone who publishes to the broker controls the actuator. "
                "Use the broker's IP (.local names do not resolve). Blank turns it off.</p>"));
        _button(PSTR("Save"));
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
            _errorPage(PSTR("Invalid broker (use host or host:port)"), PSTR("/mqtt"));
            return;
        }
        if (user.length() >= sizeof(_cfg.mqtt_user) || pass.length() >= sizeof(_cfg.mqtt_pass)) {
            _errorPage(PSTR("User or password too long"), PSTR("/mqtt"));
            return;
        }
        strlcpy(_cfg.mqtt_host, host.c_str(), sizeof(_cfg.mqtt_host));
        strlcpy(_cfg.mqtt_user, user.c_str(), sizeof(_cfg.mqtt_user));
        if (_flag("pass_clear")) _cfg.mqtt_pass[0] = '\0';
        else if (pass.length()) strlcpy(_cfg.mqtt_pass, pass.c_str(), sizeof(_cfg.mqtt_pass));
        if (!_store.save()) {
            _errorPage(PSTR("Could not save the configuration"), PSTR("/mqtt"));
            return;
        }
        if (_mqtt) _mqtt->reconnect();
        _savedPage(PSTR("MQTT settings saved!"), PSTR("/mqtt"));
    }

    void _handlePassword() {
        if (!_guard(true)) return;
        _pageBegin();
        _title(PSTR("Panel password"));
        if (_cfg.webpss.length() == 0)
            _alertP(PSTR("warning"), PSTR("Set a password to unlock the panel"));
        _p(PSTR("<form action='/password/save' method='POST'>"));
        _csrfField();
        _input(PSTR("User"), PSTR("user"), _cfg.webusr.length() ? _cfg.webusr : String("admin"),
               PSTR("required maxlength='32'"));
        _passwordInput(PSTR("New password (min. 8)"), PSTR("pass1"), PSTR("required minlength='8' maxlength='64'"));
        _passwordInput(PSTR("Confirm password"), PSTR("pass2"), PSTR("required minlength='8' maxlength='64'"));
        _button(PSTR("Save"));
        _pageEnd();
    }

    void _handlePasswordSave() {
        if (!_guard(true)) return;
        if (!_checkCSRF()) return;
        if (!_readPanelCredentials("/password")) return;
        if (!_store.save()) {
            _errorPage(PSTR("Could not save the configuration"), PSTR("/password"));
            return;
        }
        _generateCSRF(); // rotates CSRF after a credentials change
        _savedPage(PSTR("Password updated!"), PSTR("/"));
    }

    // Served straight from flash; the cache avoids resending on every page
    // (CSS and JS carry the content hash in the URL: a new version does not use
    // the old cache). The type is in RAM: it goes into the header's %s, which
    // does not read from flash.
    void _asset(const char* type, const uint8_t* data, size_t len, bool gz) {
        _server.sendHeader("Cache-Control", "max-age=604800");
        if (gz) _server.sendHeader("Content-Encoding", "gzip");
        _server.send_P(200, type, reinterpret_cast<PGM_P>(data), len);
    }

    // "UTC-03:00 Brasilia, Buenos Aires", with the saved zone selected
    void _tzSelect() {
        _selectOpen(PSTR("Time zone <span class='m'>(no daylight saving)</span>"), PSTR("tz"));
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
        return _iot.localMinute(m) ? _hhmm(m) : String("clock not synchronized");
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
    // "how long ago" from a millis(); "never" if it did not happen
    static String _ago(bool happened, unsigned long at) {
        return happened ? formatDuration(millis() - at) + " ago" : String("never");
    }

    void _go(const char* href) {
        _server.sendHeader("Location", href);
        _server.send(303, "text/plain", "");
    }

    void _handleUpdate() {
        if (!_guard()) return;
        _pageBegin(_ota.pending() ? 3 : 0);
        _otaProgressBar();

        _title(PSTR("Update"));
        _box(PSTR("Firmware"));
        _row(PSTR("Current version"), firmware_version);
        _row(PSTR("Automatic"), _cfg.ota_auto ? String("on") : String("off"));
        if (_ota.availableVersion()[0] != '\0')
            _row(PSTR("Available"), _ota.availableVersion());
        if (_ota.detail()[0] != '\0')
            _row(PSTR("State"), _ota.detail());
        _boxEnd();

        _p(PSTR("<form action='/update/auto' method='POST'>"));
        _csrfField();
        _switch(PSTR("ota_auto"), PSTR("Automatic update"), _cfg.ota_auto);
        _p(PSTR("<button class='b2'>Save</button></form>"));

        _p(PSTR("<form action='/update/check' method='POST'>"));
        _csrfField();
        _button(PSTR("Check now"));

        if (_ota.availableVersion()[0] != '\0' && !_ota.inProgress()) {
            _p(PSTR("<form action='/update/install' method='POST'>"));
            _csrfField();
            _button(PSTR("Install this version"));
        }
        _pageEnd();
    }

    void _handleUpdateAuto() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _cfg.ota_auto = _server.arg("ota_auto") == "1";
        _ota.setAutomatic(_cfg.ota_auto);
        if (!_store.save()) {
            _errorPage(PSTR("Could not save the configuration"), PSTR("/update"));
            return;
        }
        _savedPage(_cfg.ota_auto
                       ? PSTR("Automatic update on")
                       : PSTR("Automatic update off"),
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
            _errorPage(PSTR("No new version to install"), PSTR("/update"));
            return;
        }
        _ota.requestInstall();
        _go("/update");
    }

    void _deviceMark() {
        _p(PSTR("<figure><img src='/esp01.webp' alt='ESP-01' width='220' height='209'>"
                "<figcaption class='m'>Firmware for the ESP-01 module<br>"
                "Photo: SparkFun, CC BY 2.0</figcaption></figure>"));
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
        if (!none) none = PSTR("Choose");   // PSTR cannot be a default argument
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
        _selectOpen(PSTR("Type"), PSTR("sensor"));
        for (uint8_t g = 0; g < 3; g++) {
            _groupOpen(iot::SENSOR_GROUPS[g]);
            for (const iot::SensorDef& s : iot::SENSORS)
                if (s.group == g) _option(s.slug, s.label, strcmp(s.slug, current) == 0);
            _p(PSTR("</optgroup>"));
        }
        _selectEnd();
    }

    void _actuatorSelect(const char* current) {
        _selectOpen(PSTR("Type"), PSTR("actuator"));
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
            _p(PSTR("<button type='button' onclick=\"c('open')\">Open</button>"
                    "<button type='button' class='b2' onclick=\"c('close')\">Close</button>"));
        } else {
            _p(PSTR("<button type='button' role='switch' id='sw' aria-checked='"));
            _p(_iot.actuatorOn() ? PSTR("true") : PSTR("false"));
            _p(PSTR("' onclick=\"c(this.getAttribute('aria-checked')=='true'?'off':'on')\">"));
            _p(a->label);
            _p(PSTR("</button>"));
        }
        _p(PSTR("<label>Pulse (s)<input type='number' id='pd' min='1' max='3600' value='"));
        snprintf(buf, sizeof(buf), "%u", a->pulse);
        _s(buf);
        _p(PSTR("'></label><button type='button' class='b2' onclick=\"c('pulse '+$('pd').value)\">"
                "Pulse</button>"));
        if (_iot.hasRule()) {
            _p(PSTR("<button type='button' class='b2' id='au' onclick=\"c('auto')\""));
            if (!_iot.manual()) _p(PSTR(" hidden"));
            _p(PSTR(">Back to automatic</button>"));
        }
        _p(PSTR("</div>"));
        if (cap == iot::C_LEVEL || cap == iot::C_COLOR)
            _range(cap == iot::C_LEVEL ? PSTR("Level") : PSTR("Brightness"),
                   PSTR("lv"), _iot.level(), 100, PSTR("level "), PSTR("%"));
        if (cap == iot::C_POSITION)
            _range(PSTR("Angle"), PSTR("ag"), _iot.angle(), 180, PSTR("angle "), PSTR(" deg"));
        if (cap == iot::C_COLOR) {
            snprintf(buf, sizeof(buf), "%06lx", (unsigned long)_iot.color());
            _p(PSTR("<label>Color<input type='color' id='co' value='#"));
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
            _alertP(PSTR("warning"), PSTR("Type unknown to this version: that part stays off"));
        if (t.notify && _cfg.ntfy_topic[0] == '\0')
            _alertP(PSTR("warning"), PSTR("Set the ntfy topic to send the reading"));

        bool live = _iot.hasSensor() || _iot.hasActuator();
        if (live) {
            _box(PSTR("Now"));
            if (_iot.hasSensor()) _row(PSTR("Sensor"), _iot.sensorText(), PSTR("ns"));
            if (_iot.hasActuator()) {
                _row(PSTR("Actuator"), _iot.actuatorText(), PSTR("na"));
                if (_iot.hasRule())
                    _row(PSTR("Mode"), _iot.manual() ? String("manual")
                                       : _iot.inHours() ? String("automatic")
                                       : String("automatic, outside the window"),
                         PSTR("nm"));
            }
            _boxEnd();
            if (_iot.hasActuator()) _controls();
            _p(PSTR("<p id='msg' class='m' role='status' aria-live='polite'></p>"));
        }

        _p(PSTR("<figure><img src='/pinout.svg' width='1040' height='700' "
                "alt='ESP-01 pins, top view: GPIO0, RX (GPIO3), GPIO2, GND; "
                "VCC 3.3 V, RST, CH_PD, TX (GPIO1)'></figure>"));

        bool hs = t.sensor[0] && t.sensorPin >= 0;
        bool ha = t.actuator[0] && t.actuatorPin >= 0;
        _p(PSTR("<form action='/iot/save' method='POST'>"));
        _csrfField();
        _fieldsetOpen(PSTR("fp"), PSTR("What does this device do?"));
        _radio(PSTR("n"), PSTR("Nothing (IoT off)"), !hs && !ha);
        _radio(PSTR("s"), PSTR("Sensor only: measures and reports"), hs && !ha);
        _radio(PSTR("a"), PSTR("Actuator only: obeys commands"), !hs && ha);
        _radio(PSTR("sa"), PSTR("Sensor and actuator"), hs && ha);
        _p(PSTR("</fieldset>"));

        _fieldsetOpen(PSTR("fs"), PSTR("Sensor"));
        _sensorSelect(t.sensor);
        _pinSelect(PSTR("Pin"), PSTR("sensor_pin"), t.sensorPin);
        _divOpen(PSTR("sd"));
        _pair(PSTR("Active with the pin"), PSTR("sensor_high"),
              PSTR("Low"), PSTR("High"), t.sensorHigh);
        _input(PSTR("Active state name"), PSTR("sensor_on"), t.onText, PSTR("maxlength='15' placeholder='type default'"));
        _input(PSTR("Inactive state name"), PSTR("sensor_off"), t.offText,
               PSTR("maxlength='15' placeholder='type default'"));
        _p(PSTR("</div>"));
        _divOpen(PSTR("sm"));
        _pair(PSTR("Value used by the rule"), PSTR("metric"),
              PSTR("Humidity"), PSTR("Temperature"), t.metric == 1);
        _p(PSTR("</div><p class='m' id='sh'></p></fieldset>"));

        _fieldsetOpen(PSTR("fa"), PSTR("Actuator"));
        _actuatorSelect(t.actuator);
        _pinSelect(PSTR("Pin"), PSTR("actuator_pin"), t.actuatorPin);
        _divOpen(PSTR("al"));
        _pair(PSTR("On with the pin"), PSTR("actuator_high"),
              PSTR("Low (common relay modules)"), PSTR("High"), t.actuatorHigh);
        _p(PSTR("</div>"));
        _divOpen(PSTR("ap"));
        _number(PSTR("Level when on (%)"), "duty_on", t.dutyOn, 0, 100);
        _number(PSTR("Level when off (%)"), "duty_off", t.dutyOff, 0, 100);
        _p(PSTR("</div>"));
        _divOpen(PSTR("aa"));
        _number(PSTR("Open angle"), "angle_on", t.angleOn, 0, 180);
        _number(PSTR("Closed angle"), "angle_off", t.angleOff, 0, 180);
        _p(PSTR("</div>"));
        _divOpen(PSTR("ac"));
        _number(PSTR("Number of LEDs"), "leds", t.leds, 1, IoTService::MAX_LEDS);
        _p(PSTR("</div>"));
        _number(PSTR("Turn off by itself after (min; 0 = never)"), "max_on", t.maxOn, 0, 1440);
        _pair(PSTR("On power-up"), PSTR("restore"),
              PSTR("Off"), PSTR("Last command"), t.restore);
        _p(PSTR("<p class='m' id='ah'></p></fieldset>"));

        _fieldsetOpen(PSTR("fr"), PSTR("Rule"));
        _switch(PSTR("rule"), PSTR("The sensor drives the actuator (automatic mode)"), t.rule);
        _divOpen(PSTR("rd"));
        _pair(PSTR("When the sensor becomes active"), PSTR("toggle"),
              PSTR("On while active"), PSTR("Flip the actuator"), t.toggle);
        _p(PSTR("</div>"));
        _divOpen(PSTR("rn"));
        _pair(PSTR("On when the value is"), PSTR("above"),
              PSTR("Below the limit"), PSTR("Above the limit"), t.above);
        _number(PSTR("Limit"), "threshold", t.threshold, -999, 9999);
        _number(PSTR("Margin to turn off"), "margin", t.margin, 0, 50);
        _divOpen(PSTR("rl"));
        _pair(PSTR("Level"), PSTR("linear"),
              PSTR("On and off at the limit"), PSTR("Proportional up to the high limit"), t.linear);
        _divOpen(PSTR("rh"));
        _number(PSTR("High limit"), "threshold_hi", t.thresholdHi, -999, 9999);
        _p(PSTR("</div>"));
        _p(PSTR("</div></div>"));
        _divOpen(PSTR("rt"));
        _switch(PSTR("window"), PSTR("Only within a time window (local time)"), t.window);
        _divOpen(PSTR("rw"));
        _time(PSTR("From"), PSTR("win_start"), t.winStart);
        _time(PSTR("To"), PSTR("win_end"), t.winEnd);
        _p(PSTR("<p class='m'>Outside the window, automatic mode keeps the actuator off; "
                "a manual command holds until auto. The time zone is in Settings.</p>"));
        _p(PSTR("</div></div><p class='m' id='rs'></p></fieldset>"));

        _switch(PSTR("notify"), PSTR("Send readings and changes to ntfy"), t.notify);
        _button(PSTR("Save"));
        if (live) _apiBox();
        if (hs || ha) {
            _p(PSTR("<form action='/iot/clear' method='POST'>"));
            _csrfField();
            _p(PSTR("<button class='b2'>Remove sensor and actuator</button></form>"));
        }

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
        _s(_csrfToken);
        _p(PSTR("';</script><script src='/iot.js?v=" WEB_IOT_TAG "'></script>"));
        _pageEnd();
    }

    // Reads an integer from the form within the range; otherwise answers 400
    bool _int(const char* name, int min, int max, int16_t& out) {
        if (IoTService::parseI16(_server.arg(name), min, max, out)) return true;
        _errorPage(PSTR("Invalid value"), PSTR("/iot"));
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
            _errorPage(PSTR("Invalid state name"), PSTR("/iot"));
            return false;
        }
        strlcpy(dst, v.c_str(), 16);
        return true;
    }

    bool _pinArg(const char* name, int8_t& out) {
        String v = _server.arg(name);   // a temporary inside && counts one more branch
        if (IoTService::parsePin(v, out) && iot::allowedPin(out)) return true;
        _errorPage(PSTR("Choose the pin"), PSTR("/iot"));
        return false;
    }

    void _handleIotSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String role = _server.arg("role");
        bool hs = role == "s" || role == "sa";
        bool ha = role == "a" || role == "sa";
        if (!hs && !ha && role != "n") {
            _errorPage(PSTR("Choose what the device does"), PSTR("/iot"));
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
                _errorPage(PSTR("Invalid sensor type"), PSTR("/iot"));
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
                _errorPage(PSTR("Invalid actuator type"), PSTR("/iot"));
                return;
            }
            strlcpy(t.actuator, slug.c_str(), sizeof(t.actuator));
            cap = iot::ACTUATORS[i].cap;
            if (!_pinArg("actuator_pin", t.actuatorPin)) return;
            if (hs && (t.actuatorPin == t.sensorPin ||
                       (kind == iot::K_SHT3X && t.actuatorPin == I2cBus::sclFor(t.sensorPin)))) {
                _errorPage(PSTR("Sensor and actuator need different pins"), PSTR("/iot"));
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
                        _errorPage(PSTR("The high limit must be above the limit"), PSTR("/iot"));
                        return;
                    }
                }
            }
            t.window = t.rule && _flag("window");
            if (t.window) {
                String from = _server.arg("win_start");   // a temporary inside || counts one more branch
                String to = _server.arg("win_end");
                if (!parseHhmm(from, t.winStart) || !parseHhmm(to, t.winEnd)) {
                    _errorPage(PSTR("Invalid time"), PSTR("/iot"));
                    return;
                }
                if (t.winStart == t.winEnd) {
                    _errorPage(PSTR("The window needs different start and end times"), PSTR("/iot"));
                    return;
                }
            }
        }
        t.notify = _flag("notify");
        if (!PowerService::pinsFree(_cfg.bat_sda, t)) {
            _errorPage(PSTR("Pin used by the battery gauge"), PSTR("/iot"));
            return;
        }
        if ((hs || ha) && !_cfg.api_token[0]) _newToken();

        _cfg.iot = t;
        if (!_store.save()) {
            _errorPage(PSTR("Could not save the configuration"), PSTR("/iot"));
            return;
        }
        _iot.begin();
        _savedPage(PSTR("IoT set"), PSTR("/iot"));
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
            _loginFailed();
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
            _errorPage(PSTR("Could not save the configuration"), PSTR("/iot"));
            return;
        }
        _savedPage(PSTR("New token generated; the old one no longer works"), PSTR("/iot"));
    }

    // Deletes sensor, actuator, rule and token. Asks for confirmation on its
    // own page: it works without JavaScript and the screen reader announces it.
    void _handleIotClear() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        if (!_flag("confirm")) {
            _pageBegin();
            _title(PSTR("Remove"));
            _alertP(PSTR("warning"),
                    PSTR("The sensor and the actuator will be deleted with all settings and the rule. "
                         "The actuator turns off, the local API stops answering and the token stops working."));
            _p(PSTR("<form action='/iot/clear' method='POST'>"));
            _csrfField();
            _p(PSTR("<input type='hidden' name='confirm' value='1'>"
                    "<button class='bx'>Yes, remove</button></form>"));
            _backButton(PSTR("/iot"));
            _pageEnd();
            return;
        }
        _cfg.iot = IoTConfig();
        _cfg.api_token[0] = '\0';
        if (!_store.save()) {
            _errorPage(PSTR("Could not save the configuration"), PSTR("/iot"));
            return;
        }
        LittleFS.remove(IoTState::PATH);
        _iot.begin();
        _log.log(PSTR("IoT: sensor and actuator removed"));
        _savedPage(PSTR("Sensor and actuator removed"), PSTR("/iot"));
    }

    // Token and examples to integrate without internet (scripts, Home Assistant)
    void _apiBox() {
        String ip = WiFi.localIP().toString();
        _box(PSTR("Local API"));
        _p(PSTR("<div class='r'><span>No internet, same network: GET /api/state and "
                "POST /api/cmd with the Authorization: Bearer header and the token.</span></div>"));
        if (_cfg.api_token[0]) {
            _p(PSTR("<div class='r'><label>Token<input readonly value='"));
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
        _p(_cfg.api_token[0] ? PSTR("<button class='b2'>Generate new token</button></form>")
                             : PSTR("<button class='b2'>Generate token</button></form>"));
    }
};
