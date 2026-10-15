// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <cstring>
#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "OTAService.h"
#include "WebService.h"
#include "Totp.h"
#include "../web_browser.h"
#include "../i2c_sim.h"

using Args = std::map<std::string, std::string>;

static Logger logger;
static Config cfg;
static time_t webNow = 0;
static time_t webTime() { return webNow; }
static time_t nextSignIn = 0;

// RFC 4226/6238 secret. setUp copies it in, so the panel starts enrolled.
static const uint8_t PANEL_TOTP[Totp::KEY_LEN] = {
    '1','2','3','4','5','6','7','8','9','0','1','2','3','4','5','6','7','8','9','0'
};
static constexpr time_t PANEL_TIME = 1700100000;   // the fixture signs in before it

struct Fixture {
    ConfigStore store{cfg, logger};
    WiFiService wifi{cfg, logger};
    OTAService  ota{logger, wifi};
    Clock            clock{webTime};
    IoTService  iot{cfg, logger, &clock};
    NtfyNotifier     ntfy{cfg, logger, clock, &iot};
    WiFiClient       mqttNet;
    MqttService      mqtt{cfg, logger, iot, mqttNet};
    PowerService     power{cfg, logger};
    WebService  web;
    MockBrowser srv{[this]() { web.tick(); }};

    explicit Fixture(bool withNtfy = true, bool withMqtt = true, bool withPower = true)
        : web(cfg, logger, store, wifi, ota, iot, withNtfy ? &ntfy : nullptr,
              withMqtt ? &mqtt : nullptr, withPower ? &power : nullptr) {
        web.begin();
        // The sign-in stores the spent code in /config.json; the tests start
        // from the file system they set up
        auto before = MockFS::files;
        if (cfg.webpss.length() && !Totp::blank(cfg.totp)) signIn();
        MockFS::files = before;
    }

    void keepCookie() {
        auto it = srv.headers.find("Set-Cookie");
        if (it == srv.headers.end()) return;
        const std::string& v = it->second;
        auto p = v.find("sid=");
        if (p == std::string::npos) return;
        auto e = v.find(';', p);
        srv.cookie = v.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }

    // Signs in against the current totp secret. The wall clock is restored.
    // A code works once, so with the clock in 1970 each sign-in takes the
    // next step of a run that ends well before PANEL_TIME (nextSignIn): the
    // tests keep PANEL_TIME's codes for themselves.
    void signIn() {
        time_t saved = webNow;
        if (webNow <= Clock::MIN_VALID_EPOCH) {
            webNow = nextSignIn;
            nextSignIn += 30;
        }
        char code[8];
        Totp::codeAt(cfg.totp, webNow, code);
        bool was = srv.authOk;
        srv.authOk = false;
        srv.request(HTTP_POST, "/login", {
            {"user", std::string(cfg.webusr.c_str())},
            {"pass", std::string(cfg.webpss.c_str())},
            {"code", code},
            {"_csrf", web.csrfToken()},
        });
        keepCookie();
        srv.authOk = was;
        webNow = saved;
    }

    bool get(const char* path)              { return srv.request(HTTP_GET, path); }
    bool post(const char* path, Args a = {}) {
        a["_csrf"] = web.sessionCsrfToken();
        return srv.request(HTTP_POST, path, a);
    }
    const std::string& out() { return srv.out; }
    void f_iotTick() { iot.tick(millis()); }

    void enterConfigMode() {
        cfg.ssid = "";
        wifi.begin();
    }
    void startOta() {
        MockHTTP::push(200, R"({"tag_name":"v999"})");
        MockHTTP::push(200, std::string(1000, 'X'));
        ota.tick(OTAService::CHECK_INTERVAL);
        ota.tick(OTAService::CHECK_INTERVAL + 1); // 512 of 1000 bytes
    }
};

static const char* otherCode(const char* code) {
    return std::strcmp(code, "111111") == 0 ? "222222" : "111111";
}

// The password form also asks for the current password and a code. The
// clock is in 1970, so the code goes with the browser's time (t).
static Args confirmed(Args a, time_t at = PANEL_TIME) {
    char code[8];
    Totp::codeAt(cfg.totp, at, code);
    a["pass"] = cfg.webpss.c_str();
    a["code"] = code;
    a["t"]    = std::to_string((long long)at);
    return a;
}

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    cfg.webusr = "admin";
    cfg.webpss = "strong-pass";
    std::memcpy(cfg.totp, PANEL_TOTP, sizeof(cfg.totp));
    cfg.ssid   = "Home";
    webNow = 0;
    nextSignIn = PANEL_TIME - 30 * 200;
}
void tearDown() {}

// ── static helpers ───────────────────────────────────────────────────────────

void test_escape() {
    TEST_ASSERT_EQUAL_STRING("&lt;a href=&quot;x&quot;&gt;&amp;&#39;ok",
        WebService::escape("<a href=\"x\">&'ok").c_str());
}

void test_mask_secret() {
    TEST_ASSERT_EQUAL_STRING("",         WebService::maskSecret("").c_str());
    TEST_ASSERT_EQUAL_STRING("****abc",  WebService::maskSecret("abc").c_str());
    TEST_ASSERT_EQUAL_STRING("****wxyz", WebService::maskSecret("abcdwxyz").c_str());
}

void test_format_duration() {
    TEST_ASSERT_EQUAL_STRING("1h 01m 01s", WebService::formatDuration(3661000UL).c_str());
}

void test_format_bytes() {
    TEST_ASSERT_EQUAL_STRING("0 B", WebService::formatBytes(0).c_str());
    TEST_ASSERT_EQUAL_STRING("8 B", WebService::formatBytes(8).c_str());
    TEST_ASSERT_EQUAL_STRING("1000 B", WebService::formatBytes(1000).c_str());
    TEST_ASSERT_EQUAL_STRING("1 KB", WebService::formatBytes(1024).c_str());
    TEST_ASSERT_EQUAL_STRING("1.5 KB", WebService::formatBytes(1500).c_str());
    TEST_ASSERT_EQUAL_STRING("1 MB", WebService::formatBytes(1024UL * 1024UL).c_str());
    TEST_ASSERT_EQUAL_STRING("1.5 MB", WebService::formatBytes(1572864).c_str());
    TEST_ASSERT_EQUAL_STRING("1 GB", WebService::formatBytes(1024UL * 1024UL * 1024UL).c_str());
    TEST_ASSERT_EQUAL_STRING("1.5 GB", WebService::formatBytes(1610612736UL).c_str());
}

// ── startup and security ────────────────────────────────────────────────────

void test_begin_opens_port_and_makes_random_csrf() {
    Fixture f;
    TEST_ASSERT_TRUE(MockServer::begun);
    TEST_ASSERT_TRUE(logHas(logger, "Web server listening on port 80"));
    TEST_ASSERT_EQUAL(16, (int)std::strlen(f.web.csrfToken()));

    std::string first = f.web.csrfToken();
    Fixture g;
    TEST_ASSERT_TRUE(first != g.web.csrfToken());
}

void test_tick_serves_clients() {
    Fixture f;
    f.web.tick();   // nobody connected
    TEST_ASSERT_TRUE(f.get("/status"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(f.srv.conn->closed);
}

// Path or method without a route: empty 404
void test_unknown_route() {
    Fixture f;
    TEST_ASSERT_FALSE(f.get("/nada"));
    TEST_ASSERT_EQUAL(404, f.srv.code);
    TEST_ASSERT_FALSE(f.post("/status"));
    TEST_ASSERT_FALSE(f.get("/save"));
    TEST_ASSERT_FALSE(f.srv.send("PUT / HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_EQUAL(404, f.srv.code);
}

void test_without_password_redirects_to_set_password() {
    cfg.webpss = "";
    Fixture f;
    const char* paths[] = {"/", "/status", "/log", "/cloudflare", "/ntfy", "/update", "/iot"};
    for (const char* p : paths) {
        f.get(p);
        TEST_ASSERT_EQUAL(302, f.srv.code);
        TEST_ASSERT_EQUAL_STRING("/password", f.srv.headers["Location"].c_str());
    }
    f.post("/save", {{"ssid", "x"}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

void test_authentication_required() {
    Fixture f;
    f.srv.authOk = false;
    const char* paths[] = {"/", "/status", "/log", "/cloudflare", "/ntfy", "/mqtt", "/password",
                           "/update", "/iot", "/config", "/mfa"};
    for (const char* p : paths) {
        f.get(p);
        TEST_ASSERT_EQUAL(302, f.srv.code);
        TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
        TEST_ASSERT_EQUAL(0u, f.srv.headers.count("WWW-Authenticate"));
    }
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/mqtt/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install", "/iot/save", "/iot/cmd",
                           "/iot/token", "/iot/clear", "/config/save", "/mfa", "/mfa/reset"};
    for (const char* p : posts) {
        f.post(p);
        TEST_ASSERT_EQUAL(302, f.srv.code);
        TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
    }
    f.get("/login");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Sign in"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='code'"));
    TEST_ASSERT_TRUE(contains(f.out(), "autocomplete='one-time-code'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Sign out"));
}

// The old Basic header does not open the panel. The cookie does, in full.
void test_session_authentication() {
    Fixture f;
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Sign out"));
    f.get("/login");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());

    f.srv.authOk = false;
    f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", "Basic YWRtaW46c3Ryb25nLXBhc3M="}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());

    std::string good = f.srv.cookie;
    const char* bad[] = {
        "sid=0123456789abcdef0123456789abcde",          // 31
        "sid=abcd;",                                        // cut at the semicolon
        "sid=0123456789abcdef0123456789abcdef0",        // 33
        "sid=0123456789abcdef0123456789abcdef",         // other value
        "other=1",
        "a=1;   ",
        "   ",
        "",
    };
    for (const char* c : bad) {
        f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", c}});
        TEST_ASSERT_EQUAL(302, f.srv.code);
    }
    f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", "a=1; " + good + "; b=2"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", good}});
    TEST_ASSERT_EQUAL(200, f.srv.code);

    // Already signed in: the authenticator page is not the enrollment page,
    // and a bad CSRF is not even looked at
    f.srv.authOk = true;
    f.srv.request(HTTP_POST, "/mfa", {{"_csrf", "00000000deadbeef"}, {"code", "000000"}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());
    f.srv.authOk = false;

    // A failed sign-in from a browser that does not send the cookie leaves it
    std::string before = good;
    webNow = PANEL_TIME;
    char goodCode[8];
    Totp::codeAt(cfg.totp, webNow, goodCode);
    f.srv.request(HTTP_POST, "/login", {
        {"user", "admin"}, {"pass", "wrong-pass!"}, {"code", goodCode}, {"_csrf", f.web.csrfToken()}
    });
    TEST_ASSERT_EQUAL(401, f.srv.code);
    TEST_ASSERT_EQUAL(0u, f.srv.headers.count("WWW-Authenticate"));
    TEST_ASSERT_TRUE(contains(f.out(), "Wrong user, password or code"));
    TEST_ASSERT_FALSE(contains(f.out(), "Sign out"));
    f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", before}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

void test_invalid_csrf_rejected_on_every_post() {
    Fixture f;
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/mqtt/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install", "/iot/save", "/iot/cmd",
                           "/iot/token", "/iot/clear", "/config/save", "/login", "/logout", "/mfa/reset"};
    for (const char* p : posts) {
        f.srv.request(HTTP_POST, p, {{"_csrf", "00000000deadbeef"}});
        TEST_ASSERT_EQUAL(403, f.srv.code);
    }
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

// ── home page ────────────────────────────────────────────────────────────────

void test_root_station_mode() {
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Test&lt;SSID&gt;"));   // escaped SSID
    TEST_ASSERT_TRUE(contains(f.out(), "Public IP"));
    TEST_ASSERT_TRUE(contains(f.out(), "never"));
    TEST_ASSERT_TRUE(contains(f.out(), "<footer><span>dev</span></footer>"));
    TEST_ASSERT_FALSE(contains(f.out(), "OTA in progress"));
    TEST_ASSERT_FALSE(contains(f.out(), "/esp01.webp"));
    TEST_ASSERT_TRUE(contains(f.out(), "Firmware for the ESP-01 module"));
    // The photo, with the credit
    TEST_ASSERT_TRUE(contains(f.out(), "<img src='/esp01.avif' alt='ESP-01' width='220' height='209'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Photo: SparkFun, CC BY 2.0"));
}

void test_root_with_ip_and_last_update() {
    Fixture f;
    strlcpy(cfg.publicIP, "8.8.8.8", sizeof(cfg.publicIP));
    strlcpy(cfg.cf_host, "casa.exemplo.com", sizeof(cfg.cf_host));
    cfg.lastDnsUpdate = 1000;
    setMillis(61000);
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "8.8.8.8"));
    TEST_ASSERT_TRUE(contains(f.out(), "casa.exemplo.com"));
    TEST_ASSERT_TRUE(contains(f.out(), "0h 01m 00s ago"));
}

void test_root_shows_ota_progress() {
    Fixture f;
    f.startOta();
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "OTA in progress"));
    TEST_ASSERT_TRUE(contains(f.out(), "51%"));
    TEST_ASSERT_TRUE(contains(f.out(), "512 B / 1000 B"));
}

void test_root_setup_mode_first_access() {
    cfg.webpss = "";
    cfg.webusr = "";
    Fixture f;
    f.enterConfigMode();
    f.get("/");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "/esp01.webp"));
    TEST_ASSERT_TRUE(contains(f.out(), "Firmware for the ESP-01 module"));
    TEST_ASSERT_TRUE(contains(f.out(), "Setup mode"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='pass1'"));      // sets the panel password
    TEST_ASSERT_TRUE(contains(f.out(), "value='admin'"));
    TEST_ASSERT_TRUE(contains(f.out(), f.web.csrfToken()));
    // An empty SSID must not become an empty chunk (the chunked terminator)
    TEST_ASSERT_TRUE(contains(f.out(), "name='ssid' value=''"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='pass'"));
    TEST_ASSERT_TRUE(contains(f.out(), "</html>"));
    // AP mode has no internet: the style comes from the device itself, nothing external
    TEST_ASSERT_TRUE(contains(f.out(), "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<script src='/i18n.js?v=" WEB_I18N_TAG "#'></script>"));
    TEST_ASSERT_FALSE(contains(f.out(), "<script src='http"));
    TEST_ASSERT_FALSE(contains(f.out(), "https://"));
}

void test_root_setup_mode_keeps_user() {
    cfg.webpss = "";
    cfg.webusr = "joao";
    Fixture f;
    f.enterConfigMode();
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "value='joao'"));
}

void test_root_setup_mode_with_password_set() {
    Fixture f;
    f.enterConfigMode();
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "name='ssid'"));
    TEST_ASSERT_FALSE(contains(f.out(), "name='pass1'"));
}

// ── /save (Wi-Fi) ────────────────────────────────────────────────────────────

void test_save_wifi_ok() {
    Fixture f;
    f.post("/save", {{"ssid", "NewNet"}, {"pass", "12345678"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Restarting"));
    TEST_ASSERT_EQUAL_STRING("NewNet", cfg.ssid.c_str());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "NewNet"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_save_wifi_open_network() {
    Fixture f;
    f.post("/save", {{"ssid", "Open"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.pass.c_str());
}

void test_save_wifi_validation() {
    Fixture f;
    f.post("/save", {{"ssid", ""}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Invalid SSID"));

    f.post("/save", {{"ssid", std::string(33, 's')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);

    f.post("/save", {{"ssid", "Home"}, {"pass", "short"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "8 to 63"));

    f.post("/save", {{"ssid", "Home"}, {"pass", std::string(64, 'p')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);

    TEST_ASSERT_FALSE(f.wifi.restartPending());
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.size());
}

void test_save_wifi_first_access_needs_panel_password() {
    cfg.webpss = "";
    Fixture f;
    f.enterConfigMode();

    f.post("/save", {{"ssid", "Home"}, {"user", "me"}, {"pass1", "abcdefgh"}, {"pass2", "other123"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "do not match"));

    f.post("/save", {{"ssid", "Home"}, {"user", "me"}, {"pass1", "short"}, {"pass2", "short"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "8 to 64"));

    f.post("/save", {{"ssid", "Home"}, {"user", "  "}, {"pass1", "abcdefgh"}, {"pass2", "abcdefgh"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Invalid user"));
    TEST_ASSERT_EQUAL_STRING("", cfg.webpss.c_str());

    f.post("/save", {{"ssid", "Home"}, {"user", "me"}, {"pass1", "abcdefgh"}, {"pass2", "abcdefgh"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("me",       cfg.webusr.c_str());
    TEST_ASSERT_TRUE(Password::hashed(cfg.webpss));
    TEST_ASSERT_TRUE(Password::check("abcdefgh", cfg.webpss));
}

void test_save_wifi_write_fails() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/save", {{"ssid", "Rede2"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

// ── Embedded images ──────────────────────────────────────────────────────────

// Panel CSS and JS: gzipped, public (the first setup page uses them) and,
// with the content hash in the URL, cached for a year
static void assetGz(const char* path, const char* type, size_t len) {
    cfg.webpss = "";
    Fixture f;
    TEST_ASSERT_TRUE(f.get(path));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING(type, f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("gzip", f.srv.headers["Content-Encoding"].c_str());
    TEST_ASSERT_EQUAL_STRING("max-age=31536000, immutable", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL(len, f.out().size());
    TEST_ASSERT_EQUAL_HEX8(0x1f, (uint8_t)f.out()[0]);
    TEST_ASSERT_EQUAL_HEX8(0x8b, (uint8_t)f.out()[1]);
}

void test_gzipped_css_and_js() {
    assetGz("/s.css", "text/css", sizeof(WEB_CSS_GZ));
    assetGz("/iot.js", "text/javascript", sizeof(WEB_IOT_GZ));
    assetGz("/i18n.js", "text/javascript", sizeof(WEB_I18N_GZ));
    TEST_ASSERT_EQUAL(8, (int)std::strlen(WEB_CSS_TAG));
    TEST_ASSERT_EQUAL(8, (int)std::strlen(WEB_IOT_TAG));
    TEST_ASSERT_EQUAL(8, (int)std::strlen(WEB_I18N_TAG));
}

// The photo: AVIF, public (the first setup page shows it), already
// compressed (no gzip), cached for a week (no hash in the URL)
void test_device_photo_is_avif() {
    cfg.webpss = "";
    Fixture f;
    TEST_ASSERT_FALSE(f.get("/esp01.webp"));
    TEST_ASSERT_EQUAL(404, f.srv.code);
    TEST_ASSERT_TRUE(f.get("/esp01.avif"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("image/avif", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL(0u, f.srv.headers.count("Content-Encoding"));
    TEST_ASSERT_EQUAL_STRING("max-age=604800", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL(sizeof(ASSET_PHOTO), f.out().size());
    TEST_ASSERT_EQUAL_STRING("ftypavif", f.out().substr(4, 8).c_str());   // ISO BMFF brand
}

void test_embedded_gzipped_svg_pinout() {
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/pinout.svg?v=" ASSET_PINOUT_TAG));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("gzip", f.srv.headers["Content-Encoding"].c_str());
    TEST_ASSERT_EQUAL_STRING("max-age=31536000, immutable", f.srv.headers["Cache-Control"].c_str());   // hash in the URL
    TEST_ASSERT_EQUAL(sizeof(ASSET_PINOUT_GZ), f.out().size());
    TEST_ASSERT_EQUAL_HEX8(0x1f, (uint8_t)f.out()[0]);   // gzip signature
    TEST_ASSERT_EQUAL_HEX8(0x8b, (uint8_t)f.out()[1]);
}

// ── /log ─────────────────────────────────────────────────────────────────────

void test_log_escapes_content() {
    Fixture f;
    logger.log("<script>alert(1)</script>");
    f.get("/log");
    TEST_ASSERT_TRUE(contains(f.out(), "&lt;script&gt;"));
    TEST_ASSERT_FALSE(contains(f.out(), "<script>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Web server listening"));
}

// ── /status ──────────────────────────────────────────────────────────────────

void test_status_connected() {
    Fixture f;
    MockFS::put("/config.json", "{}");
    ESP._resetInfo = "Fatal <exception>";
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "http-equiv='refresh' content='30'"));
    TEST_ASSERT_TRUE(contains(f.out(), "Free heap</span><b>48.8 KB</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Largest block</span><b>39.1 KB</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Flash"));
    TEST_ASSERT_TRUE(contains(f.out(), ">1 MB<"));
    TEST_ASSERT_TRUE(contains(f.out(), ">293 KB<"));
    TEST_ASSERT_TRUE(contains(f.out(), "OTA reserve"));
    TEST_ASSERT_TRUE(contains(f.out(), ">400 KB<"));
    TEST_ASSERT_TRUE(contains(f.out(), "LittleFS"));
    TEST_ASSERT_TRUE(contains(f.out(), ">64 KB<"));
    TEST_ASSERT_TRUE(contains(f.out(), ">8 KB<"));
    TEST_ASSERT_TRUE(contains(f.out(), "dead"));               // chip id in hex
    TEST_ASSERT_TRUE(contains(f.out(), "192.168.1.1"));
    TEST_ASSERT_TRUE(contains(f.out(), "AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_TRUE(contains(f.out(), "Fatal &lt;exception&gt;"));
    TEST_ASSERT_TRUE(contains(f.out(), "<section class='c'><h2>Memory</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"));
    TEST_ASSERT_FALSE(contains(f.out(), "Temp"));
}

void test_status_disconnected_and_ota() {
    Fixture f;
    f.startOta();
    WiFi._status = WL_DISCONNECTED;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "OTA in progress"));
    TEST_ASSERT_FALSE(contains(f.out(), "192.168.1.1"));
}

// Public IP checked by the device, with the reverse name and the times
void test_status_public_ip() {
    Fixture f;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>Public IP</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Reverse</span><b>-</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Last check</span><b>never</b>"));

    strlcpy(cfg.publicIP, "200.1.2.3", sizeof(cfg.publicIP));
    strlcpy(cfg.publicHost, "200-1-2-3.isp<x>.net", sizeof(cfg.publicHost));
    cfg.ipChanged = 1000;
    cfg.ipChecked = 241000;
    setMillis(301000);
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "IP</span><b>200.1.2.3</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Reverse</span><b>200-1-2-3.isp&lt;x&gt;.net</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Changed</span><b>0h 05m 00s ago</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Last check</span><b>0h 01m 00s ago</b>"));
}

// ── /cloudflare ──────────────────────────────────────────────────────────────

void test_cloudflare_without_token() {
    Fixture f;
    f.get("/cloudflare");
    TEST_ASSERT_TRUE(contains(f.out(), "Token not set"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='record'"));
    TEST_ASSERT_FALSE(contains(f.out(), "name='record1'"));
}

void test_cloudflare_with_masked_token_and_escaped_values() {
    Fixture f;
    strlcpy(cfg.cf_token, "secret-abcd", sizeof(cfg.cf_token));
    strlcpy(cfg.cf_zone, "z'1", sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_record, "r<0>", sizeof(cfg.cf_record));
    f.get("/cloudflare");
    TEST_ASSERT_TRUE(contains(f.out(), "****abcd"));
    TEST_ASSERT_FALSE(contains(f.out(), "secret"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='z&#39;1'"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='r&lt;0&gt;'"));
}

// ── Settings: name and hostname ──────────────────────────────────────────────

void test_name_in_title_and_header() {
    Fixture f;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "<title>ESP8266 DNS</title>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b>&#9679; ESP8266 DNS</b>"));
    strlcpy(cfg.dev_name, "Sala <1>", sizeof(cfg.dev_name));
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "<title>Sala &lt;1&gt;</title>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b>&#9679; Sala &lt;1&gt;</b>"));

    f.get("/");                                       // off the home page
    TEST_ASSERT_FALSE(contains(f.out(), "action='/config/save'"));
    TEST_ASSERT_TRUE(f.get("/config"));
    TEST_ASSERT_TRUE(contains(f.out(), "<h1>Settings</h1>"));
    TEST_ASSERT_TRUE(contains(f.out(), "action='/config/save'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='name' value='Sala &lt;1&gt;'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='hostname' value=''"));
}

void test_settings_save_name_and_hostname() {
    Fixture f;
    f.mqtt.tick(0);                                   // connecting to the broker
    strlcpy(cfg.mqtt_host, "10.0.0.2", sizeof(cfg.mqtt_host));
    f.post("/config/save", {{"name", " Gate "}, {"hostname", " gate-1 "}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Settings saved!"));
    TEST_ASSERT_EQUAL_STRING("Gate", cfg.dev_name);
    TEST_ASSERT_EQUAL_STRING("gate-1", cfg.hostname);
    TEST_ASSERT_EQUAL_STRING("gate-1", WiFi.hostName.c_str());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"hostname\":\"gate-1\""));

    // Blank goes back to the default; the core hostname stays until a restart
    WiFi.hostName = "";
    Fixture g(true, false);
    g.post("/config/save", {{"name", ""}, {"hostname", ""}});
    TEST_ASSERT_EQUAL(200, g.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.dev_name);
    TEST_ASSERT_EQUAL_STRING("", cfg.hostname);
    TEST_ASSERT_EQUAL_STRING("", WiFi.hostName.c_str());
}

// Only the names the device answers to: a page on another site that points
// its own name at the device (DNS rebinding) gets 421 before any route
static int hostGet(Fixture& f, const char* host) {
    std::string r = "GET /login HTTP/1.1\r\n";
    if (host) r += std::string("Host: ") + host + "\r\n";
    f.srv.send(r + "\r\n");
    return f.srv.code;
}

void test_unknown_host_is_refused() {
    strlcpy(cfg.cf_host, "home.example.com", sizeof(cfg.cf_host));
    Fixture f;
    const char* allowed[] = {nullptr, "esp", "192.168.1.50:80", "ESP-00DEAD.local", "esp-00dead.lan",
                             "Home.Example.com"};
    for (const char* h : allowed) TEST_ASSERT_EQUAL_MESSAGE(200, hostGet(f, h), h ? h : "(none)");
    const char* refused[] = {"evil.com", "esp-00dead1.lan", "esp-00beef.lan", "esp.example.com", "1.2.3.4.evil.com", ":80",
                             "home.example.com.evil.com"};
    for (const char* h : refused) {
        TEST_ASSERT_EQUAL_MESSAGE(421, hostGet(f, h), h);
        TEST_ASSERT_TRUE(contains(f.srv.out, "Unknown address"));
    }

    // A hostname of its own replaces the core's, and Settings can list more
    strlcpy(cfg.hostname, "Garage", sizeof(cfg.hostname));
    strlcpy(cfg.web_hosts, "esp.example.com,Other.Example.org", sizeof(cfg.web_hosts));
    TEST_ASSERT_EQUAL(200, hostGet(f, "garage.local"));
    TEST_ASSERT_EQUAL(421, hostGet(f, "esp-00dead.lan"));
    TEST_ASSERT_EQUAL(200, hostGet(f, "esp.example.com"));
    TEST_ASSERT_EQUAL(200, hostGet(f, "other.example.org"));
    TEST_ASSERT_EQUAL(421, hostGet(f, "example.com"));
    TEST_ASSERT_EQUAL(421, hostGet(f, "esp.example.co"));
    TEST_ASSERT_EQUAL(421, hostGet(f, "esp.example.net"));          // as long as a listed one
}

void test_settings_save_other_addresses() {
    Fixture f;
    f.get("/config");
    TEST_ASSERT_TRUE(contains(f.out(), "name='hosts'"));
    f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"hosts", " esp.example.com,b-2.example.org "}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("esp.example.com,b-2.example.org", cfg.web_hosts);
    std::string tooLong(64, 'a');
    const char* bad[] = {"a b.com", "x.com;y.com", "evil.com/", tooLong.c_str()};
    for (const char* h : bad) {
        f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"hosts", h}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "Invalid addresses"));
    }
    TEST_ASSERT_EQUAL_STRING("esp.example.com,b-2.example.org", cfg.web_hosts);
}

void test_settings_refuse_invalid_name_or_hostname() {
    Fixture f;
    const char* names[] = {"com \"aspas\"", "barra \\", "tab\tx", "del\x7f",
                           "123456789012345678901234567890123"};
    for (const char* n : names) {
        f.post("/config/save", {{"name", n}, {"hostname", "ok"}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "Invalid name"));
    }
    const char* hosts[] = {"-a", "a-", "com espaco", "a_b", "a.b",
                           "123456789012345678901234567890123"};
    for (const char* h : hosts) {
        f.post("/config/save", {{"name", "ok"}, {"hostname", h}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "Invalid hostname"));
    }
    TEST_ASSERT_EQUAL_STRING("", cfg.dev_name);
    f.post("/config/save", {{"name", "ok"}, {"hostname", "a"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);

    MockFS::failWriteOpen = true;
    f.post("/config/save", {{"name", "x"}, {"hostname", ""}});
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
    MockFS::failWriteOpen = false;

    f.srv.request(HTTP_POST, "/config/save", {{"name", "x"}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    f.srv.authOk = false;
    f.post("/config/save", {{"name", "x"}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

void test_time_zone() {
    TEST_ASSERT_TRUE(WebService::validTz(-180));
    TEST_ASSERT_TRUE(WebService::validTz(345));               // Nepal, +05:45
    TEST_ASSERT_TRUE(WebService::validTz(840));
    TEST_ASSERT_FALSE(WebService::validTz(-200));
    TEST_ASSERT_FALSE(WebService::validTz(900));

    Fixture f;
    f.get("/config");
    TEST_ASSERT_TRUE(contains(f.out(), "<select name='tz'><option value='-720'>UTC-12:00 Baker Island</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='0' selected>UTC+00:00 London, Lisbon</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='-180'>UTC-03:00 Brasilia, Buenos Aires</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='345'>UTC+05:45 Nepal</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='840'>UTC+14:00 Kiritimati</option></select>"));

    f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"tz", "-180"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL(-180, cfg.tz);
    f.get("/config");
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='-180' selected>UTC-03:00"));
    TEST_ASSERT_FALSE(contains(f.out(), "<option value='0' selected>"));

    // Off the list or junk: refused, nothing changes
    const char* bad[] = {"-200", "900", "-3", "abc"};
    for (const char* v : bad) {
        f.post("/config/save", {{"name", "x"}, {"hostname", ""}, {"tz", v}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "Invalid time zone"));
    }
    TEST_ASSERT_EQUAL(-180, cfg.tz);
    TEST_ASSERT_EQUAL_STRING("", cfg.dev_name);
}

// Panel language: Settings saves it and the footer passes it to i18n.js,
// after the #; blank = the browser's. The page itself carries markers, not
// English (out is the English that the test browser writes back).
void test_panel_language() {
    Fixture f;
    f.get("/config");
    TEST_ASSERT_TRUE(contains(f.out(), "<select name='lang'><option value='' selected>Browser language</option>"
                                       "<option value='en'>English</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='fr'>Français</option></select>"));
    TEST_ASSERT_TRUE(contains(f.out(), "#'></script></body></html>"));
    TEST_ASSERT_TRUE(contains(f.srv.raw, L_SETTINGS));
    TEST_ASSERT_FALSE(contains(f.srv.raw, "Settings"));
    TEST_ASSERT_FALSE(contains(f.srv.raw, "Browser language"));

    f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"lang", "pt"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("pt", cfg.lang);
    f.get("/config");
    TEST_ASSERT_TRUE(contains(f.out(), "<option value=''>Browser language</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='pt' selected>Português</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<script src='/i18n.js?v=" WEB_I18N_TAG "#pt'></script>"));

    // Off the list: refused, nothing changes
    const char* bad[] = {"de", "p", "ptx", "PT"};
    for (const char* v : bad) {
        f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"lang", v}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "Invalid language"));
    }
    TEST_ASSERT_EQUAL_STRING("pt", cfg.lang);

    // Without the field (an old form): back to the browser's
    f.post("/config/save", {{"name", ""}, {"hostname", ""}});
    TEST_ASSERT_EQUAL_STRING("", cfg.lang);
}

// The footer escapes what came from the file, even if it is not a language
void test_panel_language_escaped() {
    strlcpy(cfg.lang, "'>", sizeof(cfg.lang));
    Fixture f;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "#&#39;&gt;'></script>"));
}

void test_local_time_on_status() {
    Fixture f;
    webNow = 1000;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "Local time</span><b>clock not synchronized"));
    webNow = 1704067200 + 15 * 3600 + 7 * 60;          // 15:07 UTC
    cfg.tz = -180;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "Local time</span><b>12:07"));
}

void test_iot_time_window() {
    Fixture f;
    Args a = {{"role", "sa"}, {"sensor", "pir"}, {"sensor_pin", "2"}, {"sensor_on", ""},
              {"sensor_off", ""}, {"actuator", "relay"}, {"actuator_pin", "0"}, {"max_on", "0"},
              {"rule", "1"}, {"window", "1"}, {"win_start", "22:00"}, {"win_end", "06:30"}};
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(cfg.iot.window);
    TEST_ASSERT_EQUAL(22 * 60, cfg.iot.winStart);
    TEST_ASSERT_EQUAL(6 * 60 + 30, cfg.iot.winEnd);

    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "id='window' name='window' value='1' checked>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='win_start' value='22:00'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='win_end' value='06:30'"));
    // Clock in 1970: outside the window
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='nm'>automatic, outside the window</b>"));
    webNow = 1704067200 + 23 * 3600;
    f.f_iotTick();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='nm'>automatic</b>"));

    const char* bad[][2] = {{"24:00", "06:00"}, {"2200", "06:00"}, {"22:0a", "06:00"},
                            {"22:60", "06:00"}, {"22:00", ""}, {"22-00", "06:00"}, {"2/:00", "06:00"},
                            {"22:00", "6:000"}, {"07:00", "07:00"}};
    for (auto& b : bad) {
        Args x = a;
        x["win_start"] = b[0];
        x["win_end"] = b[1];
        f.post("/iot/save", x);
        TEST_ASSERT_EQUAL(400, f.srv.code);
    }
    TEST_ASSERT_TRUE(contains(f.out(), "different start and end times"));

    // Without the rule, the window does not apply; the old times stay
    a.erase("rule");
    a["win_start"] = "junk";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(cfg.iot.window);
    TEST_ASSERT_EQUAL(22 * 60, cfg.iot.winStart);
}

void test_iot_sht3x_uses_two_pins() {
    Fixture f;
    Args a = {{"role", "sa"}, {"sensor", "sht3x"}, {"sensor_pin", "0"}, {"metric", "1"},
              {"actuator", "relay"}, {"actuator_pin", "2"}, {"max_on", "0"}};
    f.post("/iot/save", a);                               // actuator on SCL
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "need different pins"));
    a["actuator_pin"] = "3";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("sht3x", cfg.iot.sensor);
    TEST_ASSERT_EQUAL(1, cfg.iot.metric);
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='sht3x' selected>Temperature and humidity (SHT30/SHT31, I2C)"));
    TEST_ASSERT_TRUE(contains(f.out(), "sht3x:7"));
}

// ── Power: battery gauge ─────────────────────────────────────────────────────

// Simulated MAX17048 on RX (SDA) and TX (SCL)
struct Gauge {
    I2cSim   sim{3, 1};
    uint16_t soc = 0x4C00, rate = 0, cell = 51200;
    Gauge() {
        sim.present = {PowerService::ADDR};
        sim.reply = [this](uint8_t, const std::vector<uint8_t>& w) {
            uint16_t v = w[1] == 0x04 ? soc : w[1] == 0x16 ? rate : cell;
            return std::vector<uint8_t>{(uint8_t)(v >> 8), (uint8_t)v};
        };
    }
};

void test_power_in_header() {
    Fixture f;
    f.get("/status");                                     // no gauge: on supply
    TEST_ASSERT_TRUE(contains(f.out(), "</b><span class='pw' title='On supply'>&#9889; Power</span><nav"));
    TEST_ASSERT_FALSE(contains(f.out(), "<h2>Battery</h2>"));

    cfg.bat_sda = 3;
    f.power.begin();
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "title='Gauge not answering'>&#128267; Battery ?</span>"));
    TEST_ASSERT_TRUE(contains(f.out(), "State</span><b>gauge not answering"));
    TEST_ASSERT_TRUE(contains(f.out(), "Charge</span><b>-"));

    Gauge g;
    g.rate = (uint16_t)-25;
    f.power.tick(0);
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "<span class='pw' title='On battery'>&#128267; Battery 76%</span>"));
    TEST_ASSERT_TRUE(contains(f.out(), "State</span><b>on battery"));
    TEST_ASSERT_TRUE(contains(f.out(), "Charge</span><b>76%"));
    TEST_ASSERT_TRUE(contains(f.out(), "Voltage</span><b>4.00 V"));
    TEST_ASSERT_TRUE(contains(f.out(), "Rate</span><b>-5.2 %/h"));

    g.soc = 0x1300;                                       // 19 %: highlighted
    f.power.tick(PowerService::READ_MS);
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "<span class='pw lo' title='On battery'>&#128267; Battery 19%</span>"));

    g.rate = 50;
    f.power.tick(2 * PowerService::READ_MS);
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "<span class='pw' title='Battery charging'>&#9728; Charging 19%</span>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Rate</span><b>10.4 %/h"));

    // Without the service (OTA recovery), nothing at the top
    Fixture noPower(true, true, false);
    noPower.get("/status");
    TEST_ASSERT_FALSE(contains(noPower.out(), "class='pw"));
}

void test_settings_save_gauge() {
    Fixture f;
    f.get("/config");
    TEST_ASSERT_TRUE(contains(f.out(), "<select name='bat_sda'><option value='-1' selected>None</option>"));
    f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"bat_sda", "3"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL(3, cfg.bat_sda);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"bat_sda\":3"));
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[1]);   // begin again: SCL released

    // Invalid pin, or taken by IoT
    f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"bat_sda", "7"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    cfg.iot.actuatorPin = 1;
    f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"bat_sda", "3"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "The gauge pins are taken"));
    f.post("/config/save", {{"name", ""}, {"hostname", ""}, {"bat_sda", "-1"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL(-1, cfg.bat_sda);
    f.post("/config/save", {{"name", ""}, {"hostname", ""}});   // without the field: no gauge
    TEST_ASSERT_EQUAL(-1, cfg.bat_sda);

    Fixture noPower(true, true, false);
    noPower.post("/config/save", {{"name", "x"}, {"hostname", ""}});
    TEST_ASSERT_EQUAL(200, noPower.srv.code);
}

// IoT cannot take the gauge pins, except an SHT3x on the same pair
void test_iot_respects_gauge() {
    cfg.bat_sda = 3;
    Fixture f;
    Args a = {{"role", "a"}, {"actuator", "relay"}, {"actuator_pin", "1"}, {"max_on", "0"}};
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Pin used by the battery gauge"));
    a["actuator_pin"] = "0";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(200, f.srv.code);
    f.post("/iot/save", {{"role", "s"}, {"sensor", "sht3x"}, {"sensor_pin", "3"}, {"metric", "0"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

// Cloudflare IDs: 32 hexadecimal digits
static const char* ZONE_ID   = "023e105f4ecef8ad9ca31a8372d0c353";
static const char* RECORD_ID = "372e67954025e0ba6aaa6d586b9e0b59";

void test_cloudflare_save_ok() {
    Fixture f;
    f.post("/cloudflare/save", {
        {"token", "NOVO_TOKEN-x"}, {"zone", std::string(" ") + ZONE_ID + " "}, {"host", "h.com"},
        {"record", std::string(" ") + RECORD_ID + " "}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("NOVO_TOKEN-x", cfg.cf_token);
    TEST_ASSERT_EQUAL_STRING(ZONE_ID, cfg.cf_zone);
    TEST_ASSERT_EQUAL_STRING("h.com", cfg.cf_host);
    TEST_ASSERT_EQUAL_STRING(RECORD_ID, cfg.cf_record);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], RECORD_ID));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "NOVO_TOKEN"));
}

void test_cloudflare_save_empty_token_keeps_current() {
    Fixture f;
    strlcpy(cfg.cf_token, "OLD", sizeof(cfg.cf_token));
    f.post("/cloudflare/save", {{"zone", ZONE_ID}, {"record", RECORD_ID}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("OLD", cfg.cf_token);
    f.post("/cloudflare/save", {{"zone", ""}, {"record", ""}});     // empty: not set up
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.cf_zone);
}

// The IDs and the token go into the API request: only the characters they
// can have, so nothing changes the path or adds a header
void test_cloudflare_save_rejects_odd_ids_and_token() {
    Fixture f;
    strlcpy(cfg.cf_zone, ZONE_ID, sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_token, "OLD", sizeof(cfg.cf_token));
    const std::string hex31(31, 'a');
    const char* badIds[] = {"ZONA", "023e105f4ecef8ad9ca31a8372d0c35g", "023e105f/4ecef8ad9ca31a8372d0c35",
                            "../dns_records/023e105f4ecef8ad9"};
    for (const char* id : badIds) {
        f.post("/cloudflare/save", {{"zone", id}, {"record", RECORD_ID}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "32 hexadecimal digits"));
        f.post("/cloudflare/save", {{"zone", ZONE_ID}, {"record", id}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
    }
    f.post("/cloudflare/save", {{"zone", hex31}, {"record", RECORD_ID}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    const char* badTokens[] = {"a b", "tok%0d%0aX-Evil: 1", "tok/../x", "t\xc3\xa9"};
    for (const char* t : badTokens) {
        f.post("/cloudflare/save", {{"token", t}, {"zone", ZONE_ID}, {"record", RECORD_ID}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "Invalid token"));
    }
    f.post("/cloudflare/save", {{"token", std::string("a\r\nb")}, {"zone", ZONE_ID}, {"record", RECORD_ID}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING(ZONE_ID, cfg.cf_zone);
    TEST_ASSERT_EQUAL_STRING("OLD", cfg.cf_token);
}

void test_cloudflare_save_long_value_rejected_unchanged() {
    Fixture f;
    strlcpy(cfg.cf_zone, "ORIGINAL", sizeof(cfg.cf_zone));
    f.post("/cloudflare/save", {{"zone", ZONE_ID}, {"record", std::string(40, 'r')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "too long"));
    TEST_ASSERT_EQUAL_STRING("ORIGINAL", cfg.cf_zone);

    const char* fields[] = {"token", "zone", "host", "record"};
    for (const char* k : fields) {
        f.post("/cloudflare/save", {{k, std::string(64, 'x')}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
    }
}

void test_cloudflare_save_write_fails() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/cloudflare/save", {{"zone", ZONE_ID}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
}

// ── /ntfy ────────────────────────────────────────────────────────────────────

void test_ntfy_page() {
    Fixture f;
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "Topic not set"));

    strlcpy(cfg.ntfy_topic, "meu_topico", sizeof(cfg.ntfy_topic));
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "Topic: meu_topico"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='meu_topico'"));
}

void test_ntfy_save() {
    Fixture f;
    f.post("/ntfy/save", {{"topic", "a/../b"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Invalid topic"));

    f.post("/ntfy/save", {{"topic", " new-topic "}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("new-topic", cfg.ntfy_topic);

    f.post("/ntfy/save", {{"topic", ""}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.ntfy_topic);
}

void test_ntfy_save_write_fails() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/ntfy/save", {{"topic", "x"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
}

// ── /mqtt ────────────────────────────────────────────────────────────────────

void test_mqtt_page() {
    Fixture f;
    f.get("/mqtt");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "<b>off</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "esp8266iot/00dead/state, /cmd, /avail"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='host' value=''"));

    strlcpy(cfg.mqtt_host, "10.0.0.2:1884", sizeof(cfg.mqtt_host));
    strlcpy(cfg.mqtt_user, "casa", sizeof(cfg.mqtt_user));
    strlcpy(cfg.mqtt_pass, "secret", sizeof(cfg.mqtt_pass));
    f.get("/mqtt");
    TEST_ASSERT_TRUE(contains(f.out(), "value='10.0.0.2:1884'"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='casa'"));
    TEST_ASSERT_FALSE(contains(f.out(), "secret"));          // the password never comes back to the page
    TEST_ASSERT_TRUE(contains(f.out(), "<b>connecting</b>"));
}

void test_mqtt_page_without_service() {
    Fixture f(true, false);
    f.get("/mqtt");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "State"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='host'"));
    f.post("/mqtt/save", {{"host", "10.0.0.2"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

void test_mqtt_save() {
    Fixture f;
    f.post("/mqtt/save", {{"host", " 10.0.0.2:1884 "}, {"user", " casa "}, {"pass", "s3nha"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "MQTT settings saved!"));
    TEST_ASSERT_EQUAL_STRING("10.0.0.2:1884", cfg.mqtt_host);
    TEST_ASSERT_EQUAL_STRING("casa", cfg.mqtt_user);
    TEST_ASSERT_EQUAL_STRING("s3nha", cfg.mqtt_pass);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"mqtt_host\":\"10.0.0.2:1884\""));

    // A blank password keeps it; the switch deletes it
    f.post("/mqtt/save", {{"host", "10.0.0.2"}, {"user", "casa"}, {"pass", ""}});
    TEST_ASSERT_EQUAL_STRING("s3nha", cfg.mqtt_pass);
    f.post("/mqtt/save", {{"host", "10.0.0.2"}, {"user", "casa"}, {"pass_clear", "1"}});
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt_pass);

    // Blank turns it off
    f.post("/mqtt/save", {{"host", ""}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt_host);
}

void test_mqtt_save_validation() {
    Fixture f;
    f.post("/mqtt/save", {{"host", "10.0.0.2:99999"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Invalid broker"));
    f.post("/mqtt/save", {{"host", std::string(64, 'h')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);

    f.post("/mqtt/save", {{"host", "b"}, {"user", std::string(32, 'u')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "User or password too long"));
    f.post("/mqtt/save", {{"host", "b"}, {"pass", std::string(64, 'p')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt_host);

    // At the limit it still counts
    f.post("/mqtt/save", {{"host", std::string(63, 'h')}, {"user", std::string(31, 'u')},
                          {"pass", std::string(63, 'p')}});
    TEST_ASSERT_EQUAL(200, f.srv.code);

    MockFS::failWriteOpen = true;
    f.post("/mqtt/save", {{"host", "b"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));

    // Without CSRF
    MockFS::failWriteOpen = false;
    f.srv.request(HTTP_POST, "/mqtt/save", {{"host", "x"}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
}

// ── /password ────────────────────────────────────────────────────────────────

void test_password_page_first_access_without_auth() {
    cfg.webpss = "";
    cfg.webusr = "";
    Fixture f;
    f.srv.authOk = false; // must not be consulted
    f.get("/password");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Set a password"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='admin'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Replace authenticator"));
}

void test_password_page_with_password() {
    Fixture f;
    f.get("/password");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "Set a password"));
    TEST_ASSERT_TRUE(contains(f.out(), "Replace authenticator"));
}

void test_password_save_rotates_csrf() {
    Fixture f;
    std::string old = f.web.csrfToken();
    f.post("/password/save", confirmed({{"user", "new"}, {"pass1", "secret123"}, {"pass2", "secret123"}}));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("new",       cfg.webusr.c_str());
    TEST_ASSERT_TRUE(Password::hashed(cfg.webpss));
    TEST_ASSERT_TRUE(Password::check("secret123", cfg.webpss));
    TEST_ASSERT_TRUE(old != f.web.csrfToken());
    TEST_ASSERT_TRUE(contains(f.out(), "url=/login"));
    f.get("/status");
    TEST_ASSERT_EQUAL(302, f.srv.code);   // the old cookie was cleared
}

// The session cookie alone (readable on plain HTTP) does not change the
// password: the current one and a code, each wrong one a counted mistake
void test_password_save_needs_current_password_and_code() {
    Fixture f;
    f.get("/password");
    TEST_ASSERT_TRUE(contains(f.out(), "Current password"));
    TEST_ASSERT_TRUE(contains(f.out(), "autocomplete='one-time-code'"));
    Args np{{"user", "admin"}, {"pass1", "secret123"}, {"pass2", "secret123"}};

    f.post("/password/save", np);                          // nothing: no time
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "clock not synchronized"));
    Args a = confirmed(np);
    a["pass"] = "wrong-pass!";
    f.post("/password/save", a);
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Wrong user, password or code"));
    a = confirmed(np);
    a["code"] = otherCode(a["code"].c_str());
    f.post("/password/save", a);
    TEST_ASSERT_EQUAL(400, f.srv.code);
    a = confirmed(np);
    a.erase("code");
    f.post("/password/save", a);
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(Password::check("strong-pass", cfg.webpss));
    for (int i = 3; i < WebService::FREE_LOGIN_FAILS; i++) {
        a = confirmed(np);
        a["pass"] = "wrong-pass!";
        f.post("/password/save", a);
    }
    f.post("/password/save", confirmed(np));
    TEST_ASSERT_EQUAL(429, f.srv.code);
    TEST_ASSERT_TRUE(Password::check("strong-pass", cfg.webpss));

    setMillis(WebService::LOGIN_WAIT_MAX_MS);
    f.post("/password/save", confirmed(np));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(Password::hashed(cfg.webpss));
    TEST_ASSERT_TRUE(Password::check("secret123", cfg.webpss));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(PANEL_TIME / 30), cfg.totpBrowserStep);
    TEST_ASSERT_EQUAL_MEMORY(PANEL_TOTP, cfg.totp, sizeof(PANEL_TOTP));   // the key stays
}

// The first password: there is none to confirm, so no current password or code
void test_password_save_first_password() {
    cfg.webpss = "";
    cfg.webusr = "";
    std::memset(cfg.totp, 0, sizeof(cfg.totp));
    Fixture f;
    f.post("/password/save", {{"user", "admin"}, {"pass1", "secret123"}, {"pass2", "secret123"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("admin",     cfg.webusr.c_str());
    TEST_ASSERT_TRUE(Password::hashed(cfg.webpss));
    TEST_ASSERT_TRUE(Password::check("secret123", cfg.webpss));
}

// A blank key never matches, even inside a session: its codes are public
void test_blank_key_code_never_matches() {
    Fixture f;
    std::memset(cfg.totp, 0, sizeof(cfg.totp));
    f.post("/password/save", confirmed({{"user", "admin"}, {"pass1", "secret123"}, {"pass2", "secret123"}}));
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Wrong user, password or code"));
    TEST_ASSERT_TRUE(Password::check("strong-pass", cfg.webpss));
}

// The menu only where its pages open: signed in with the authenticator, or
// the first setup on the access point. Its links come from /i18n.js.
void test_menu_only_where_it_opens() {
    Fixture f;
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "<nav id='mn'><form class='out'"));

    f.srv.authOk = false;                                // the sign-in page
    f.srv.cookie.clear();
    f.get("/login");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "<nav"));
    TEST_ASSERT_TRUE(contains(f.out(), "</header><main>"));

    std::memset(cfg.totp, 0, sizeof(cfg.totp));          // authenticator not set up yet
    f.signIn();
    f.srv.authOk = true;
    f.get("/mfa");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "<nav><form class='out'"));   // sign-out only
    TEST_ASSERT_FALSE(contains(f.out(), "id='mn'"));
}

void test_menu_on_first_setup() {
    cfg.webpss = "";
    cfg.webusr = "";
    Fixture f;
    f.get("/password");                                  // station mode: only this page opens
    TEST_ASSERT_FALSE(contains(f.out(), "<nav"));
    f.enterConfigMode();                                 // access point: every page opens
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "<nav id='mn'></nav>"));
}

void test_password_save_invalid() {
    Fixture f;
    // Each with its own code: a confirmed code is spent even when the new
    // password is then refused
    f.post("/password/save", confirmed({{"user", "u"}, {"pass1", "secret123"}}));
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "do not match"));
    f.post("/password/save", confirmed({{"user", std::string(33, 'u')}, {"pass1", "secret123"}, {"pass2", "secret123"}},
                                       PANEL_TIME + 30));
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Invalid user"));
    f.post("/password/save", confirmed({{"user", "u"}, {"pass1", std::string(65, 'p')}, {"pass2", std::string(65, 'p')}},
                                       PANEL_TIME + 60));
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "8 to 64"));
    TEST_ASSERT_TRUE(Password::check("strong-pass", cfg.webpss));
}

// ── /update ──────────────────────────────────────────────────────────────────

void test_update_initial_page() {
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/update"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Automatic update"));
    TEST_ASSERT_TRUE(contains(f.out(), " checked"));
    TEST_ASSERT_TRUE(contains(f.out(), "on"));
    TEST_ASSERT_TRUE(contains(f.out(), "Check now"));
    TEST_ASSERT_TRUE(contains(f.out(), "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"));
    TEST_ASSERT_FALSE(contains(f.out(), "http-equiv='refresh'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Install this version"));
}

void test_update_automatic_off() {
    cfg.ota_auto = false;
    Fixture f;
    f.ota.setAutomatic(false);
    f.get("/update");
    TEST_ASSERT_TRUE(contains(f.out(), "off"));
    TEST_ASSERT_FALSE(contains(f.out(), " checked"));
}

void test_update_saves_automatic() {
    Fixture f;
    f.post("/update/auto", {});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(cfg.ota_auto);
    TEST_ASSERT_FALSE(f.ota.automatic());
    TEST_ASSERT_TRUE(contains(f.out(), "off"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"ota_auto\":false"));

    f.post("/update/auto", {{"ota_auto", "1"}});
    TEST_ASSERT_TRUE(cfg.ota_auto);
    TEST_ASSERT_TRUE(f.ota.automatic());
    TEST_ASSERT_TRUE(contains(f.out(), "on"));
}

void test_update_auto_write_fails() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/update/auto", {{"ota_auto", "1"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
}

void test_update_check_redirects_and_shows_result() {
    Fixture f;
    f.post("/update/check");
    TEST_ASSERT_EQUAL(303, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/update", f.srv.headers["Location"].c_str());
    TEST_ASSERT_TRUE(f.ota.pending());

    f.get("/update");
    TEST_ASSERT_TRUE(contains(f.out(), "http-equiv='refresh' content='3'"));
    TEST_ASSERT_TRUE(contains(f.out(), "Checking GitHub..."));
    TEST_ASSERT_FALSE(contains(f.out(), "Install this version"));

    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(0);
    f.get("/update");
    TEST_ASSERT_TRUE(contains(f.out(), "v999"));
    TEST_ASSERT_TRUE(contains(f.out(), "Install this version"));
    TEST_ASSERT_FALSE(contains(f.out(), "http-equiv='refresh'"));
}

void test_update_install_without_version() {
    Fixture f;
    f.post("/update/install");
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "No new version"));
}

void test_update_install_shows_progress() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    MockHTTP::push(200, std::string(1000, 'X'));
    f.post("/update/install");
    TEST_ASSERT_EQUAL(303, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/update", f.srv.headers["Location"].c_str());

    f.ota.tick(1);
    f.get("/update");
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_TRUE(contains(f.out(), "OTA in progress"));
    TEST_ASSERT_TRUE(contains(f.out(), "http-equiv='refresh' content='3'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Install this version"));
}

void test_password_save_write_fails() {
    Fixture f;
    std::string old = f.web.csrfToken();
    MockFS::failWriteOpen = true;
    f.post("/password/save", confirmed({{"user", "u"}, {"pass1", "secret123"}, {"pass2", "secret123"}}));
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(old == f.web.csrfToken());
}

// ── /iot ─────────────────────────────────────────────────────────────────────

static void sensorCfg(const char* slug, int pin) {
    strlcpy(cfg.iot.sensor, slug, sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = (int8_t)pin;
}

static void actuatorCfg(const char* slug, int pin) {
    strlcpy(cfg.iot.actuator, slug, sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = (int8_t)pin;
}

void test_iot_page_with_nothing_set() {
    Fixture f;
    f.get("/iot");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "<h1>IoT</h1>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<img src='/pinout.svg?v=" ASSET_PINOUT_TAG "'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='n' checked>Nothing"));
    TEST_ASSERT_TRUE(contains(f.out(), "<legend>What does this device do?</legend>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<optgroup label='Temperature and humidity'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<optgroup label='Color'><option value='strip'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='-1' selected>Choose</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "var SK={dht22:3,dht11:2"));
    TEST_ASSERT_TRUE(contains(f.out(), "strip:4},T='"));
    // Signed in: the session's token, not the one the sign-in page shows
    TEST_ASSERT_TRUE(contains(f.out(), f.web.sessionCsrfToken()));
    TEST_ASSERT_FALSE(contains(f.out(), f.web.csrfToken()));
    TEST_ASSERT_TRUE(contains(f.out(), "';</script><script src='/iot.js?v=" WEB_IOT_TAG "'></script></body></html>"));
    // i18n.js before iot.js: the form script uses L() and the translated names
    TEST_ASSERT_TRUE(f.out().find("/i18n.js?v=") < f.out().find("var SK={"));
    // With nothing in use: no "Now" card (iot.js only reads live with ns/na)
    TEST_ASSERT_FALSE(contains(f.out(), "<h2>Now</h2>"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='ns'"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='na'"));
    TEST_ASSERT_FALSE(contains(f.out(), "http-equiv='refresh'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Set the ntfy topic"));
    TEST_ASSERT_FALSE(contains(f.out(), "Type unknown"));
}

void test_iot_page_sensor_and_actuator_with_live_reading() {
    sensorCfg("pir", 2);
    actuatorCfg("relay", 0);
    cfg.iot.notify = true;
    Fixture f;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "Set the ntfy topic"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='sa' checked>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='pir' selected>Motion (PIR)</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='relay' selected>Relay</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='2' selected>GPIO2</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>Now</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='ns'>waiting</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='na'>off</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='nm'>automatic</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "role='switch' id='sw' aria-checked='false'"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Relay</button>"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='pd' min='1' max='3600' value='1'"));
    TEST_ASSERT_TRUE(contains(f.out(), "onclick=\"c('auto')\" hidden>"));
    TEST_ASSERT_TRUE(contains(f.out(), "role='status' aria-live='polite'"));
    TEST_ASSERT_TRUE(contains(f.out(), "<script src='/iot.js?v=" WEB_IOT_TAG "'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='rule' value='1' checked>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='notify' value='1' checked>"));

    // Turned on by hand: the button is checked and "back to automatic" shows
    f.iot.command("on", 0);
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "aria-checked='true'"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='nm'>manual</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "onclick=\"c('auto')\">Back"));

    strlcpy(cfg.ntfy_topic, "topic", sizeof(cfg.ntfy_topic));
    f.get("/iot");
    TEST_ASSERT_FALSE(contains(f.out(), "Set the ntfy topic"));
}

void test_iot_page_sensor_only_and_actuator_only() {
    sensorCfg("ds18b20", 3);
    Fixture f;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='s' checked>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='ns'>"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='na'"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='sw'"));

    cfg = Config{};
    cfg.webusr = "admin";
    cfg.webpss = "strong-pass";
    actuatorCfg("fan", 0);
    cfg.iot.dutyOn = 80;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='a' checked>"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='ns'"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='nm'"));
    TEST_ASSERT_FALSE(contains(f.out(), "c('auto')"));
    TEST_ASSERT_TRUE(contains(f.out(),
        "<label>Level: <output id='lvo'>80</output>%<input type='range' id='lv' "
        "min='0' max='100' value='80'"));
    TEST_ASSERT_TRUE(contains(f.out(), "c('level '+this.value)"));
    TEST_ASSERT_FALSE(contains(f.out(), "type='color'"));
}

void test_iot_position_and_color_controls() {
    actuatorCfg("servo", 2);
    cfg.iot.angleOff = 15;
    Fixture f;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "onclick=\"c('open')\">Open</button>"));
    TEST_ASSERT_TRUE(contains(f.out(), "onclick=\"c('close')\">Close</button>"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='sw'"));
    TEST_ASSERT_TRUE(contains(f.out(), "<output id='ago'>15</output> deg"));
    TEST_ASSERT_TRUE(contains(f.out(), "max='180' value='15'"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='pd' min='1' max='3600' value='3'"));

    actuatorCfg("strip", 1);
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "<label>Brightness: <output id='lvo'>100</output>%"));
    TEST_ASSERT_TRUE(contains(f.out(), "<input type='color' id='co' value='#ffffff'"));
    TEST_ASSERT_TRUE(contains(f.out(), "c('color '+this.value)"));
}

void test_iot_unknown_type_warns() {
    sensorCfg("laser", 2);
    Fixture f;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "Type unknown to this version"));
    // The configuration stays marked as a sensor: saving does not lose it by accident
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='s' checked>"));

    // A type without a pin does not count as in use
    sensorCfg("pir", -1);
    actuatorCfg("relay", -1);
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='n' checked>"));
}

void test_iot_now_as_json() {
    sensorCfg("pir", 2);
    actuatorCfg("relay", 0);
    Fixture f;
    mock::pinIn[2] = HIGH;
    f.iot.begin();
    f.iot.tick(0);
    f.iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(f.get("/iot/now"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("application/json", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("no-store", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_TRUE(contains(f.out(), "\"sensor\":{\"type\":\"pir\",\"status\":\"ok\""));
    TEST_ASSERT_TRUE(contains(f.out(), "\"on\":true,\"mode\":\"auto\""));

    f.srv.authOk = false;   // reading the device needs the login
    f.get("/iot/now");
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

void test_iot_cmd_from_panel() {
    actuatorCfg("relay", 0);
    Fixture f;
    f.iot.begin();
    TEST_ASSERT_TRUE(f.post("/iot/cmd", {{"cmd", "on"}}));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "\"on\":true"));
    TEST_ASSERT_TRUE(f.iot.actuatorOn());

    f.post("/iot/cmd", {{"cmd", "voar"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"invalid command\"}", f.out().c_str());
    f.post("/iot/cmd", {{"cmd", "angle 30"}});
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"command not valid for this actuator\"}", f.out().c_str());

    actuatorCfg("", -1);
    f.iot.begin();
    f.post("/iot/cmd", {{"cmd", "on"}});
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"no actuator configured\"}", f.out().c_str());
}

// Full sensor and actuator form; each test changes what it needs
static Args iotForm() {
    return {
        {"role", "sa"}, {"sensor", "pir"}, {"sensor_pin", "2"}, {"sensor_high", "1"},
        {"sensor_on", ""}, {"sensor_off", ""}, {"metric", "0"},
        {"actuator", "relay"}, {"actuator_pin", "0"}, {"actuator_high", "0"},
        {"duty_on", "100"}, {"duty_off", "0"}, {"angle_on", "90"}, {"angle_off", "0"},
        {"leds", "8"}, {"max_on", "0"}, {"restore", "0"}, {"rule", "1"},
        {"toggle", "0"}, {"above", "0"}, {"threshold", "0"}, {"margin", "0"},
        {"linear", "0"}, {"threshold_hi", "0"},
    };
}

static void saveExpectError(Fixture& f, Args a, const char* msg) {
    IoTConfig before = cfg.iot;
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL_MESSAGE(400, f.srv.code, msg);
    TEST_ASSERT_TRUE_MESSAGE(contains(f.out(), msg), msg);
    TEST_ASSERT_EQUAL_STRING(before.sensor, cfg.iot.sensor);
    TEST_ASSERT_EQUAL_STRING(before.actuator, cfg.iot.actuator);
}

void test_iot_save_digital_sensor_and_actuator() {
    Fixture f;
    Args a = iotForm();
    a["sensor_on"] = " molhado ";
    a["sensor_off"] = "seco-1.0,x_y";
    a["sensor_high"] = "0";
    a["actuator_high"] = "1";
    a["toggle"] = "1";
    a["max_on"] = "30";
    a["restore"] = "1";
    a["notify"] = "1";
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT set"));
    const IoTConfig& t = cfg.iot;
    TEST_ASSERT_EQUAL_STRING("pir", t.sensor);
    TEST_ASSERT_EQUAL(2, t.sensorPin);
    TEST_ASSERT_FALSE(t.sensorHigh);
    TEST_ASSERT_EQUAL_STRING("molhado", t.onText);
    TEST_ASSERT_EQUAL_STRING("seco-1.0,x_y", t.offText);
    TEST_ASSERT_EQUAL_STRING("relay", t.actuator);
    TEST_ASSERT_EQUAL(0, t.actuatorPin);
    TEST_ASSERT_TRUE(t.actuatorHigh);
    TEST_ASSERT_EQUAL(30, t.maxOn);
    TEST_ASSERT_TRUE(t.restore);
    TEST_ASSERT_TRUE(t.rule);
    TEST_ASSERT_TRUE(t.toggle);
    TEST_ASSERT_TRUE(t.notify);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"sensor\":\"pir\""));
    TEST_ASSERT_TRUE(f.iot.hasRule());                 // already applied

    // No rule: the digital sensor mode is not read
    a["rule"] = "0";
    a["toggle"] = "0";
    f.post("/iot/save", a);
    TEST_ASSERT_FALSE(cfg.iot.rule);
    TEST_ASSERT_TRUE(cfg.iot.toggle);
}

void test_iot_save_numeric_with_proportional_level() {
    Fixture f;
    Args a = iotForm();
    a["sensor"] = "dht22";
    a["metric"] = "1";
    a["actuator"] = "fan";
    a["duty_on"] = "90";
    a["duty_off"] = "10";
    a["above"] = "1";
    a["threshold"] = "-5";
    a["margin"] = "3";
    a["linear"] = "1";
    a["threshold_hi"] = "30";
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT set"));
    const IoTConfig& t = cfg.iot;
    TEST_ASSERT_EQUAL(1, t.metric);
    TEST_ASSERT_EQUAL(90, t.dutyOn);
    TEST_ASSERT_EQUAL(10, t.dutyOff);
    TEST_ASSERT_TRUE(t.above);
    TEST_ASSERT_EQUAL(-5, t.threshold);
    TEST_ASSERT_EQUAL(3, t.margin);
    TEST_ASSERT_TRUE(t.linear);
    TEST_ASSERT_EQUAL(30, t.thresholdHi);

    // Proportional only exists for level, and only when asked
    a["actuator"] = "relay";
    f.post("/iot/save", a);
    TEST_ASSERT_FALSE(cfg.iot.linear);
    a["actuator"] = "motor";
    a["linear"] = "0";
    f.post("/iot/save", a);
    TEST_ASSERT_FALSE(cfg.iot.linear);
    a["linear"] = "1";

    // DHT11 also reads the value choice; other sensors do not
    a["sensor"] = "dht11";
    a["metric"] = "0";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(0, cfg.iot.metric);
    a["sensor"] = "distance";
    a["metric"] = "9";
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT set"));
}

void test_iot_save_position_color_and_roles() {
    Fixture f;
    Args a = iotForm();
    a["role"] = "a";
    a["actuator"] = "servo";
    a["angle_on"] = "170";
    a["angle_off"] = "5";
    a["sensor"] = "junk";                              // ignored: actuator only
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT set"));
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.sensor);
    TEST_ASSERT_EQUAL(-1, cfg.iot.sensorPin);
    TEST_ASSERT_EQUAL(170, cfg.iot.angleOn);
    TEST_ASSERT_EQUAL(5, cfg.iot.angleOff);

    a["actuator"] = "strip";
    a["leds"] = "60";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(60, cfg.iot.leds);

    a["role"] = "s";
    a["sensor"] = "contact";
    a["actuator_pin"] = "2";                           // the actuator pin does not count
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT set"));
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.actuator);
    TEST_ASSERT_EQUAL(-1, cfg.iot.actuatorPin);

    a["role"] = "n";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.sensor);
    TEST_ASSERT_EQUAL(60, cfg.iot.leds);               // settings are kept
}

void test_iot_save_rejects_values() {
    Fixture f;
    struct { const char* key; const char* value; const char* msg; } cases[] = {
        {"role", "x", "Choose what the device does"},
        {"sensor", "laser", "Invalid sensor type"},
        {"sensor_pin", "-1", "Choose the pin"},
        {"sensor_pin", "7", "Choose the pin"},
        {"sensor_on", "0123456789abcdef", "Invalid state name"},
        {"sensor_off", "<b>", "Invalid state name"},
        {"actuator", "teleport", "Invalid actuator type"},
        {"actuator_pin", "-1", "Choose the pin"},
        {"actuator_pin", "2", "need different pins"},
        {"max_on", "1441", "Invalid value"},
        {"threshold", "10000", "Invalid value"},
        {"margin", "51", "Invalid value"},
    };
    for (const auto& c : cases) {
        Args a = iotForm();
        a["sensor"] = "ds18b20";
        a[c.key] = c.value;
        if (strcmp(c.key, "sensor_on") == 0 || strcmp(c.key, "sensor_off") == 0)
            a["sensor"] = "pir";
        saveExpectError(f, a, c.msg);
    }

    Args dht = iotForm();
    dht["sensor"] = "dht22";
    dht["metric"] = "2";
    saveExpectError(f, dht, "Invalid value");

    const char* levelKeys[] = {"duty_on", "duty_off"};
    for (const char* k : levelKeys) {
        Args a = iotForm();
        a["actuator"] = "motor";
        a[k] = "101";
        saveExpectError(f, a, "Invalid value");
    }
    const char* angleKeys[] = {"angle_on", "angle_off"};
    for (const char* k : angleKeys) {
        Args a = iotForm();
        a["actuator"] = "servo";
        a[k] = "181";
        saveExpectError(f, a, "Invalid value");
    }
    Args leds = iotForm();
    leds["actuator"] = "strip";
    leds["leds"] = "0";
    saveExpectError(f, leds, "Invalid value");

    Args hi = iotForm();
    hi["sensor"] = "distance";
    hi["actuator"] = "led";
    hi["linear"] = "1";
    hi["threshold"] = "50";
    hi["threshold_hi"] = "abc";
    saveExpectError(f, hi, "Invalid value");
    hi["threshold_hi"] = "50";
    saveExpectError(f, hi, "The high limit must be above the limit");
}

void test_iot_save_write_fails() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/iot/save", iotForm());
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
    TEST_ASSERT_FALSE(f.iot.hasActuator());            // the running output does not change
}

// ── Local API ────────────────────────────────────────────────────────────────

static const char* TOKEN = "0123456789abcdef0123456789abcdef";

static bool api(Fixture& f, int method, const char* path, const char* auth, Args a = {}) {
    std::map<std::string, std::string> h;
    if (auth) h["Authorization"] = auth;
    return f.srv.request(method, path, a, h);
}

void test_api_exists_only_with_iot_set() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    Fixture f;
    TEST_ASSERT_TRUE(api(f, HTTP_GET, "/api/state", "Bearer 0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_EQUAL(404, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"no sensor or actuator configured\"}", f.out().c_str());
    api(f, HTTP_POST, "/api/cmd", "Bearer 0123456789abcdef0123456789abcdef", {{"cmd", "on"}});
    TEST_ASSERT_EQUAL(404, f.srv.code);
}

void test_api_requires_token() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    sensorCfg("pir", 2);
    Fixture f;
    f.iot.begin();
    const char* bad[] = {
        nullptr,
        "Bearer 0123456789abcdef0123456789abcdee",     // last character
        "Bearer 0123456789abcdef",                     // short
        "Basic 0123456789abcdef0123456789abcdef",      // another scheme
        "bearer 0123456789abcdef0123456789abcdef",
    };
    for (const char* auth : bad) {
        api(f, HTTP_GET, "/api/state", auth);
        TEST_ASSERT_EQUAL(401, f.srv.code);
        TEST_ASSERT_EQUAL_STRING("Bearer", f.srv.headers["WWW-Authenticate"].c_str());
        TEST_ASSERT_EQUAL_STRING("{\"error\":\"invalid token\"}", f.out().c_str());
        api(f, HTTP_GET, "/api/state", (std::string("Bearer ") + TOKEN).c_str());              // resets the failure count
    }
    // The panel login does not work on the API
    TEST_ASSERT_TRUE(f.srv.authOk);
    api(f, HTTP_GET, "/api/state", nullptr);
    TEST_ASSERT_EQUAL(401, f.srv.code);

    // Without a generated token nobody gets in, not even with an empty "Bearer "
    cfg.api_token[0] = '\0';
    api(f, HTTP_GET, "/api/state", "Bearer ");
    TEST_ASSERT_EQUAL(401, f.srv.code);
}

void test_api_reads_and_commands() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("relay", 0);
    Fixture f;
    f.iot.begin();
    const char* auth = "Bearer 0123456789abcdef0123456789abcdef";

    api(f, HTTP_GET, "/api/state", auth);
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("application/json", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("no-store", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_TRUE(contains(f.out(), "\"api\":1,\"sensor\":null"));
    TEST_ASSERT_TRUE(contains(f.out(), "\"type\":\"relay\""));

    api(f, HTTP_POST, "/api/cmd", auth, {{"cmd", "on"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(f.iot.actuatorOn());

    // curl -d off: the core puts the body in "plain"
    api(f, HTTP_POST, "/api/cmd", auth, {{"plain", "off"}});
    TEST_ASSERT_FALSE(f.iot.actuatorOn());

    api(f, HTTP_POST, "/api/cmd", auth, {{"plain", "50%"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"command not valid for this actuator\"}", f.out().c_str());
    api(f, HTTP_POST, "/api/cmd", auth);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"invalid command\"}", f.out().c_str());
}

void test_api_token_generated_shown_and_replaced() {
    Fixture f;
    f.post("/iot/save", iotForm());
    TEST_ASSERT_EQUAL(32, (int)std::strlen(cfg.api_token));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], cfg.api_token));
    std::string first = cfg.api_token;

    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>Local API</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), ("readonly value='" + first + "'").c_str()));
    TEST_ASSERT_TRUE(contains(f.out(),
        ("curl -H 'Authorization: Bearer " + first + "' http://192.168.1.1/api/state").c_str()));
    TEST_ASSERT_TRUE(contains(f.out(), "' -d on http://192.168.1.1/api/cmd"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Generate new token</button>"));

    // Saving again keeps the token
    f.post("/iot/save", iotForm());
    TEST_ASSERT_EQUAL_STRING(first.c_str(), cfg.api_token);

    f.post("/iot/token");
    TEST_ASSERT_TRUE(contains(f.out(), "New token generated"));
    TEST_ASSERT_EQUAL(32, (int)std::strlen(cfg.api_token));
    TEST_ASSERT_FALSE(first == cfg.api_token);

    MockFS::failWriteOpen = true;
    f.post("/iot/token");
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
}

void test_api_without_token_and_iot_off() {
    // IoT off does not generate a token
    Fixture f;
    Args a = iotForm();
    a["role"] = "n";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL_STRING("", cfg.api_token);
    f.get("/iot");
    TEST_ASSERT_FALSE(contains(f.out(), "Local API"));

    // Set up by migration (no token): offers to generate one
    actuatorCfg("relay", 0);
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>Local API</h2>"));
    TEST_ASSERT_FALSE(contains(f.out(), "curl"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Generate token</button>"));
}

void test_ntfy_commands_page_and_validation() {
    Fixture f;
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>Commands over ntfy</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), "State</span><b>off</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='cmd' value=''"));
    TEST_ASSERT_TRUE(contains(f.out(), "Generate random name"));
    TEST_ASSERT_TRUE(contains(f.out(), "crypto.getRandomValues"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='token'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='token_clear' value='1'>"));

    struct { const char* topic; const char* cmd; const char* token; const char* msg; } bad[] = {
        {"data", "a b", "", "Invalid command topic"},
        {"same-quite-long-topic", "same-quite-long-topic", "", "must be different"},
        {"data", "cmd-curto-de-19-ch", "", "Short command topic"},
        {"data", "cmd-x", "tk-1", "Invalid ntfy token"},
        {"data", "cmd-x", "tk_0123456789012345678901234567890123456789012345", "Invalid ntfy token"},
    };
    for (const auto& b : bad) {
        f.post("/ntfy/save", {{"topic", b.topic}, {"cmd", b.cmd}, {"token", b.token}});
        TEST_ASSERT_EQUAL_MESSAGE(400, f.srv.code, b.msg);
        TEST_ASSERT_TRUE_MESSAGE(contains(f.out(), b.msg), b.msg);
    }
    TEST_ASSERT_EQUAL_STRING("", cfg.ntfy_cmd);

    f.post("/ntfy/save", {{"topic", "data"}, {"cmd", " cmd-x "}, {"token", "tk_abc"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("cmd-x", cfg.ntfy_cmd);
    TEST_ASSERT_EQUAL_STRING("tk_abc", cfg.ntfy_token);
    f.get("/ntfy");
    TEST_ASSERT_FALSE(contains(f.out(), "tk_abc"));        // the secret does not come back to the page

    // Blank keeps it; checking deletes it
    f.post("/ntfy/save", {{"topic", "data"}, {"cmd", ""}, {"token", ""}});
    TEST_ASSERT_EQUAL_STRING("tk_abc", cfg.ntfy_token);
    TEST_ASSERT_EQUAL_STRING("", cfg.ntfy_cmd);
    f.post("/ntfy/save", {{"topic", "data"}, {"token", ""}, {"token_clear", "1"}});
    TEST_ASSERT_EQUAL_STRING("", cfg.ntfy_token);

    // With an actuator and a topic, shows the connection
    strlcpy(cfg.iot.actuator, "relay", sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));
    f.iot.begin();
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "State</span><b>connecting</b>"));
}

void test_ntfy_page_without_command_service() {
    Fixture f(false);
    f.get("/ntfy");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "Commands over ntfy"));
}

void test_iot_remove_asks_for_confirmation() {
    Fixture f;
    Args a = iotForm();
    a["restore"] = "1";
    f.post("/iot/save", a);
    f.post("/iot/cmd", {{"cmd", "on"}});
    TEST_ASSERT_TRUE(f.iot.actuatorOn());
    TEST_ASSERT_TRUE(MockFS::files.count("/iot.state") == 1);
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(),
        "<form action='/iot/clear' method='POST'>"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Remove sensor and actuator</button>"));

    // The first step only shows the warning; nothing changes
    f.post("/iot/clear");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "<h1>Remove</h1>"));
    TEST_ASSERT_TRUE(contains(f.out(), "will be deleted"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='confirm' value='1'"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Yes, remove</button>"));
    TEST_ASSERT_TRUE(contains(f.out(), "href='/iot'>Back</a>"));
    TEST_ASSERT_EQUAL_STRING("pir", cfg.iot.sensor);
    TEST_ASSERT_TRUE(f.iot.actuatorOn());

    // Confirmed: deletes everything, turns the actuator off and the API goes away
    std::string token = cfg.api_token;
    f.post("/iot/clear", {{"confirm", "1"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Sensor and actuator removed"));
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.actuator);
    TEST_ASSERT_EQUAL(-1, cfg.iot.sensorPin);
    TEST_ASSERT_EQUAL(-1, cfg.iot.actuatorPin);
    TEST_ASSERT_EQUAL_STRING("", cfg.api_token);
    TEST_ASSERT_FALSE(contains(MockFS::files["/config.json"], token.c_str()));
    TEST_ASSERT_TRUE(MockFS::files.count("/iot.state") == 0);
    TEST_ASSERT_FALSE(f.iot.hasSensor());
    TEST_ASSERT_FALSE(f.iot.hasActuator());
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);  // relay active low: off

    f.get("/iot");
    TEST_ASSERT_FALSE(contains(f.out(), "/iot/clear"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='n' checked>Nothing"));
}

void test_iot_remove_write_fails() {
    Fixture f;
    f.post("/iot/save", iotForm());
    MockFS::failWriteOpen = true;
    f.post("/iot/clear", {{"confirm", "1"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
    TEST_ASSERT_TRUE(f.iot.hasActuator());  // what is running does not change
}

void test_status_shows_ap_password() {
    Fixture f;
    MockFS::put(WiFiService::AP_KEY_PATH, "abcd2345wxyz");
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "AP password ESP_Config"));
    TEST_ASSERT_TRUE(contains(f.out(), "abcd2345wxyz"));
    f.srv.authOk = false;                                     // only with the login
    f.get("/status");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
}

// ── Hardening for remote access ──────────────────────────────────────────────

static bool postLogin(Fixture& f, const char* user, const char* pass, const char* code) {
    bool was = f.srv.authOk;
    f.srv.authOk = false;
    Args a{{"user", user}, {"pass", pass}, {"_csrf", f.web.csrfToken()}};
    if (code) a["code"] = code;
    bool ok = f.srv.request(HTTP_POST, "/login", a);
    f.srv.authOk = was;
    return ok;
}

void test_login_repeated_failures_wait_longer() {
    Fixture f;
    f.srv.authOk = false;
    webNow = PANEL_TIME;
    char good[8];
    Totp::codeAt(PANEL_TOTP, webNow, good);
    const char* bad = std::strcmp(good, "000000") == 0 ? "000001" : "000000";
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) {
        postLogin(f, "admin", "strong-pass", bad);
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    TEST_ASSERT_TRUE(logHas(logger, "Login: repeated failures"));

    // While waiting, not even the right code is tried
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(429, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Too many attempts"));
    f.get("/status");
    TEST_ASSERT_EQUAL(429, f.srv.code);
    setMillis(WebService::LOGIN_WAIT_MS - 1);
    postLogin(f, "admin", "strong-pass", bad);
    TEST_ASSERT_EQUAL(429, f.srv.code);
    setMillis(WebService::LOGIN_WAIT_MS);
    postLogin(f, "admin", "nope-nope", bad);          // 6th failure: the wait doubles
    TEST_ASSERT_EQUAL(401, f.srv.code);
    unsigned long t = WebService::LOGIN_WAIT_MS;
    setMillis(t + 2 * WebService::LOGIN_WAIT_MS - 1);
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(429, f.srv.code);
    setMillis(t + 2 * WebService::LOGIN_WAIT_MS);
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(302, f.srv.code);               // getting it right resets the count
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());

    // good is spent now; the next step's code signs in again
    webNow += 30;
    Totp::codeAt(PANEL_TOTP, webNow, good);
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS - 1; i++) {
        postLogin(f, "other", "strong-pass", good);
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

void test_login_wait_has_cap_and_request_without_credentials_does_not_count() {
    Fixture f;
    f.srv.authOk = false;
    webNow = PANEL_TIME;
    char good[8];
    Totp::codeAt(PANEL_TOTP, webNow, good);
    // Opening a page without a cookie is not a mistake
    for (int i = 0; i < 20; i++) {
        f.get("/status");
        TEST_ASSERT_EQUAL(302, f.srv.code);
    }
    TEST_ASSERT_FALSE(logHas(logger, "Login: repeated failures"));

    // While the clock is in 1970 the form explains that, and it does not count
    webNow = 0;
    for (int i = 0; i < 20; i++) {
        postLogin(f, "admin", "wrong-pass!", "000000");
        TEST_ASSERT_EQUAL(200, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "clock not synchronized"));
    }
    TEST_ASSERT_FALSE(logHas(logger, "Login: repeated failures"));
    webNow = PANEL_TIME;

    unsigned long t = 0;
    for (int i = 0; i < 260; i++) {                           // until the counter stops at 255
        postLogin(f, "admin", "nope-nope!", good);
        t += WebService::LOGIN_WAIT_MAX_MS;                   // always after the wait
        setMillis(t);
    }
    postLogin(f, "admin", "nope-nope!", good);
    setMillis(t + WebService::LOGIN_WAIT_MAX_MS - 1);
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(429, f.srv.code);
    setMillis(t + WebService::LOGIN_WAIT_MAX_MS);
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(302, f.srv.code);                       // never more than 5 min
}

void test_wrong_api_token_counts_as_failure() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("relay", 0);
    Fixture f;
    f.iot.begin();
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) {
        api(f, HTTP_GET, "/api/state", "Bearer ffffffffffffffffffffffffffffffff");
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    api(f, HTTP_GET, "/api/state", (std::string("Bearer ") + TOKEN).c_str());
    TEST_ASSERT_EQUAL(429, f.srv.code);
    setMillis(WebService::LOGIN_WAIT_MS);
    api(f, HTTP_GET, "/api/state", (std::string("Bearer ") + TOKEN).c_str());
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

// Someone else on the network guessing does not hold the owner out: the
// address of the last sign-in has its own count, and the API token another
void test_guessing_elsewhere_does_not_lock_the_owner_out() {
    const uint32_t OWNER = 0x0A01A8C0, OTHER = 0x6301A8C0, THIRD = 0x6401A8C0;
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("relay", 0);
    Fixture f;                                        // signed in from OWNER
    f.iot.begin();
    webNow = PANEL_TIME;
    char good[8];
    Totp::codeAt(PANEL_TOTP, webNow, good);
    const char* bad = std::strcmp(good, "000000") == 0 ? "000001" : "000000";
    const std::string bearer = std::string("Bearer ") + TOKEN;

    f.srv.ip = OTHER;
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) {
        postLogin(f, "admin", "wrong-pass!", bad);
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(429, f.srv.code);
    f.get("/status");                                 // even with the owner's cookie
    TEST_ASSERT_EQUAL(429, f.srv.code);
    api(f, HTTP_GET, "/api/state", bearer.c_str());   // the API counts apart
    TEST_ASSERT_EQUAL(200, f.srv.code);

    // The owner browses and signs in as usual
    f.srv.ip = OWNER;
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.keepCookie();                                   // a sign-in replaces the session
    f.srv.ip = THIRD;                                 // a third address shares the others' count
    f.get("/status");
    TEST_ASSERT_EQUAL(429, f.srv.code);

    // Wrong API tokens wait on their own and leave the panel alone
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) {
        api(f, HTTP_GET, "/api/state", "Bearer ffffffffffffffffffffffffffffffff");
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    api(f, HTTP_GET, "/api/state", bearer.c_str());
    TEST_ASSERT_EQUAL(429, f.srv.code);
    f.srv.ip = OWNER;
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);

    // The owner's own mistakes wait too
    webNow += 30;
    Totp::codeAt(PANEL_TOTP, webNow, good);
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) postLogin(f, "admin", "wrong-pass!", bad);
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(429, f.srv.code);

    // After the waits, a sign-in from another address makes it the owner's
    setMillis(WebService::LOGIN_WAIT_MAX_MS);
    f.srv.ip = OTHER;
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.keepCookie();
    f.srv.ip = OWNER;                                 // now one of the others
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) postLogin(f, "admin", "wrong-pass!", bad);
    f.srv.ip = OTHER;
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

void test_ntfy_short_command_topic_without_token() {
    Fixture f;
    std::string shortTopic(WebService::MIN_CMD_TOPIC - 1, 'c');
    std::string longTopic(WebService::MIN_CMD_TOPIC, 'c');
    f.post("/ntfy/save", {{"topic", "data"}, {"cmd", shortTopic}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Short command topic"));

    f.post("/ntfy/save", {{"topic", "data"}, {"cmd", longTopic}});
    TEST_ASSERT_EQUAL(200, f.srv.code);

    // With a token (new or already saved), the short one is fine
    f.post("/ntfy/save", {{"topic", "data"}, {"cmd", shortTopic}, {"token", "tk_abc"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    f.post("/ntfy/save", {{"topic", "data"}, {"cmd", shortTopic}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    // Deleting the token and keeping the short one is not
    f.post("/ntfy/save", {{"topic", "data"}, {"cmd", shortTopic}, {"token_clear", "1"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("tk_abc", cfg.ntfy_token);
}

void test_ntfy_warns_short_topic_already_saved() {
    Fixture f;
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));       // from before the rule
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "Short command topic and no token"));
    strlcpy(cfg.ntfy_token, "tk_abc", sizeof(cfg.ntfy_token));
    f.get("/ntfy");
    TEST_ASSERT_FALSE(contains(f.out(), "and no token"));
    cfg.ntfy_token[0] = '\0';
    strlcpy(cfg.ntfy_cmd, std::string(WebService::MIN_CMD_TOPIC, 'c').c_str(), sizeof(cfg.ntfy_cmd));
    f.get("/ntfy");
    TEST_ASSERT_FALSE(contains(f.out(), "and no token"));
}

void test_security_headers() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("relay", 0);
    Fixture f;
    f.iot.begin();
    f.get("/status");
    TEST_ASSERT_EQUAL_STRING("no-store", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL_STRING("DENY", f.srv.headers["X-Frame-Options"].c_str());
    TEST_ASSERT_EQUAL_STRING("nosniff", f.srv.headers["X-Content-Type-Options"].c_str());
    api(f, HTTP_GET, "/api/state", (std::string("Bearer ") + TOKEN).c_str());
    TEST_ASSERT_EQUAL_STRING("DENY", f.srv.headers["X-Frame-Options"].c_str());
    TEST_ASSERT_EQUAL_STRING("no-store", f.srv.headers["Cache-Control"].c_str());
}

void test_log_says_where_the_command_came_from() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("relay", 0);
    Fixture f;
    f.iot.begin();
    f.post("/iot/cmd", {{"cmd", "on"}});
    TEST_ASSERT_TRUE(logHas(logger, "Panel: command \"on\""));
    api(f, HTTP_POST, "/api/cmd", (std::string("Bearer ") + TOKEN).c_str(), {{"cmd", "off"}});
    TEST_ASSERT_TRUE(logHas(logger, "API: command \"off\""));
}

static bool secretFrom(const std::string& html, uint8_t key[Totp::KEY_LEN]) {
    const char* mark = "class='k'>";
    auto p = html.find(mark);
    if (p == std::string::npos) return false;
    p += std::strlen(mark);
    auto e = html.find('<', p);
    return Totp::fromBase32(html.substr(p, e == std::string::npos ? std::string::npos : e - p).c_str(), key);
}

// No authenticator yet: the password opens only the enrollment page, and the
// key is stored after a code the clock can check.
void test_authenticator_is_required() {
    std::memset(cfg.totp, 0, sizeof(cfg.totp));
    Fixture f;
    f.srv.authOk = false;
    f.get("/login");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "not set up yet"));
    TEST_ASSERT_FALSE(contains(f.out(), "name='code'"));

    postLogin(f, "admin", "wrong-pass!", nullptr);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Wrong user, password or code"));

    postLogin(f, "admin", "strong-pass", nullptr);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/mfa", f.srv.headers["Location"].c_str());
    TEST_ASSERT_TRUE(contains(f.srv.headers["Set-Cookie"], "HttpOnly"));
    TEST_ASSERT_TRUE(contains(f.srv.headers["Set-Cookie"], "SameSite=Strict"));
    TEST_ASSERT_TRUE(contains(f.srv.headers["Set-Cookie"], "Max-Age=43200"));
    TEST_ASSERT_FALSE(contains(f.srv.headers["Set-Cookie"], "Secure"));
    TEST_ASSERT_TRUE(logHas(logger, "Panel: signed in"));
    f.keepCookie();
    f.srv.authOk = true;

    f.get("/status");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/mfa", f.srv.headers["Location"].c_str());
    f.get("/login");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/mfa", f.srv.headers["Location"].c_str());

    // The key does not exist until the page is drawn, so a code cannot match
    webNow = PANEL_TIME;
    f.post("/mfa", {{"code", "000000"}});
    TEST_ASSERT_EQUAL(401, f.srv.code);
    TEST_ASSERT_TRUE(Totp::blank(cfg.totp));
    uint8_t key[Totp::KEY_LEN];
    TEST_ASSERT_TRUE(secretFrom(f.out(), key));
    TEST_ASSERT_TRUE(contains(f.out(), "class='k'"));
    TEST_ASSERT_FALSE(contains(f.out(), "otpauth://"));

    f.get("/mfa");
    uint8_t again[Totp::KEY_LEN];
    TEST_ASSERT_TRUE(secretFrom(f.out(), again));
    TEST_ASSERT_EQUAL_MEMORY(key, again, Totp::KEY_LEN);

    webNow = 0;
    f.post("/mfa", {{"code", "123456"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "clock not synchronized"));
    TEST_ASSERT_TRUE(Totp::blank(cfg.totp));
    TEST_ASSERT_FALSE(logHas(logger, "Login: repeated failures"));

    webNow = PANEL_TIME;
    char code[8];
    Totp::codeAt(key, webNow, code);
    f.post("/mfa", {{"code", otherCode(code)}});
    TEST_ASSERT_EQUAL(401, f.srv.code);
    TEST_ASSERT_TRUE(Totp::blank(cfg.totp));

    MockFS::failWriteOpen = true;
    f.post("/mfa", {{"code", code}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
    TEST_ASSERT_TRUE(Totp::blank(cfg.totp));
    MockFS::failWriteOpen = false;

    std::string setupCookie = f.srv.cookie;
    f.post("/mfa", {{"code", code}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());
    TEST_ASSERT_EQUAL_MEMORY(key, cfg.totp, Totp::KEY_LEN);
    TEST_ASSERT_TRUE(logHas(logger, "Panel: authenticator on"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"totp_step\":"));

    // The signed-in session is a new cookie: one seen during setup is worth nothing
    f.keepCookie();
    TEST_ASSERT_TRUE(setupCookie != f.srv.cookie);
    f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", setupCookie}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "secret="));
    f.get("/mfa");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());
    f.post("/mfa", {{"code", code}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());
    TEST_ASSERT_EQUAL_MEMORY(key, cfg.totp, Totp::KEY_LEN);
}

void test_login_pages_without_password_go_to_setup() {
    cfg.webpss = "";
    Fixture f;
    f.get("/login");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/password", f.srv.headers["Location"].c_str());
    f.post("/login", {{"user", "admin"}, {"pass", "secret123"}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/password", f.srv.headers["Location"].c_str());
    f.get("/mfa");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/password", f.srv.headers["Location"].c_str());
}

void test_enroll_lockout() {
    std::memset(cfg.totp, 0, sizeof(cfg.totp));
    Fixture f;
    f.srv.authOk = false;
    webNow = PANEL_TIME;
    postLogin(f, "admin", "strong-pass", nullptr);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.keepCookie();
    f.srv.authOk = true;
    f.get("/mfa");
    uint8_t key[Totp::KEY_LEN];
    TEST_ASSERT_TRUE(secretFrom(f.out(), key));
    char code[8];
    Totp::codeAt(key, webNow, code);
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) {
        f.post("/mfa", {{"code", otherCode(code)}});
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    f.get("/mfa");
    TEST_ASSERT_EQUAL(429, f.srv.code);
    f.get("/login");
    TEST_ASSERT_EQUAL(429, f.srv.code);
    TEST_ASSERT_TRUE(Totp::blank(cfg.totp));
}

void test_code_window() {
    Fixture f;
    f.srv.authOk = false;
    webNow = PANEL_TIME;
    char cur[8], prev[8], next[8], old[8];
    Totp::codeAt(cfg.totp, webNow, cur);
    Totp::codeAt(cfg.totp, webNow - 30, prev);
    Totp::codeAt(cfg.totp, webNow + 30, next);
    Totp::codeAt(cfg.totp, webNow - 60, old);
    TEST_ASSERT_TRUE(std::strcmp(old, cur) != 0);
    TEST_ASSERT_TRUE(std::strcmp(old, prev) != 0);
    TEST_ASSERT_TRUE(std::strcmp(old, next) != 0);
    postLogin(f, "admin", "strong-pass", old);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    // One step either side; each code once, so in the order of the steps
    postLogin(f, "admin", "strong-pass", prev);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    postLogin(f, "admin", "strong-pass", cur);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());
    postLogin(f, "admin", "strong-pass", next);
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

// A code that signed in is refused after that: a second time, after the
// clock is pushed back (SNTP has no authentication), and after a reboot.
void test_code_works_once() {
    Fixture f;
    f.srv.authOk = false;
    webNow = PANEL_TIME;
    char cur[8], prev[8];
    Totp::codeAt(cfg.totp, webNow, cur);
    Totp::codeAt(cfg.totp, webNow - 30, prev);
    postLogin(f, "admin", "strong-pass", cur);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    uint32_t spent = cfg.totpStep;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(PANEL_TIME / 30), spent);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"],
                              ("\"totp_step\":" + std::to_string(spent)).c_str()));

    postLogin(f, "admin", "strong-pass", cur);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    postLogin(f, "admin", "strong-pass", prev);        // older than the last one
    TEST_ASSERT_EQUAL(401, f.srv.code);
    webNow = PANEL_TIME - 3600;                         // the clock moved back
    char back[8];
    Totp::codeAt(cfg.totp, webNow, back);
    postLogin(f, "admin", "strong-pass", back);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    TEST_ASSERT_EQUAL_UINT32(spent, cfg.totpStep);

    // Reboot: the step comes back from /config.json
    webNow = PANEL_TIME;
    cfg.totpStep = 0;
    TEST_ASSERT_TRUE(f.store.load());
    TEST_ASSERT_EQUAL_UINT32(spent, cfg.totpStep);
    Fixture g;
    g.srv.authOk = false;
    postLogin(g, "admin", "strong-pass", cur);
    TEST_ASSERT_EQUAL(401, g.srv.code);

    // A wrong password with the right code does not spend it
    webNow = PANEL_TIME + 30;
    char next[8];
    Totp::codeAt(cfg.totp, webNow, next);
    postLogin(g, "admin", "wrong-pass!", next);
    TEST_ASSERT_EQUAL(401, g.srv.code);
    TEST_ASSERT_EQUAL_UINT32(spent, cfg.totpStep);
    postLogin(g, "admin", "strong-pass", next);
    TEST_ASSERT_EQUAL(302, g.srv.code);
    TEST_ASSERT_EQUAL_UINT32(spent + 1, cfg.totpStep);

    // The save failing does not stop the sign-in; the step stays in RAM
    webNow = PANEL_TIME + 60;
    Totp::codeAt(cfg.totp, webNow, next);
    MockFS::failWriteOpen = true;
    postLogin(g, "admin", "strong-pass", next);
    TEST_ASSERT_EQUAL(302, g.srv.code);
    TEST_ASSERT_EQUAL_UINT32(spent + 2, cfg.totpStep);
    MockFS::failWriteOpen = false;
    postLogin(g, "admin", "strong-pass", next);
    TEST_ASSERT_EQUAL(401, g.srv.code);
}

// No clock (setup access point, no internet): the browser's time, sent by
// i18n.js, checks the code. It cannot bring back a spent code, and the
// device clock wins once it is synchronized.
// A browser clock that ran a year ahead, used while the device had no time,
// does not lock the authenticator once SNTP sets the real one; codes
// spent against the real clock still never come back
void test_browser_clock_ahead_does_not_lock_out() {
    Fixture f;
    f.srv.authOk = false;
    const time_t ahead = PANEL_TIME + 365L * 24 * 3600;
    auto login = [&](time_t codeAt, const std::string& t) {
        char c[8];
        Totp::codeAt(cfg.totp, codeAt, c);
        f.srv.request(HTTP_POST, "/login", {{"user", "admin"}, {"pass", "strong-pass"}, {"code", c},
                                            {"t", t}, {"_csrf", f.web.csrfToken()}});
    };
    webNow = 0;
    login(ahead, std::to_string((long long)ahead));
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(ahead / 30), cfg.totpBrowserStep);
    // Still without a clock: the browser's step holds, even for a right clock
    login(PANEL_TIME, std::to_string((long long)PANEL_TIME));
    TEST_ASSERT_EQUAL(401, f.srv.code);

    // SNTP sets the real time: that step lies in the future, and is ignored
    webNow = PANEL_TIME;
    login(PANEL_TIME, "");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(PANEL_TIME / 30), cfg.totpStep);
    login(PANEL_TIME, "");                            // spent, against the real clock
    TEST_ASSERT_EQUAL(401, f.srv.code);

    // A browser step that is not ahead still counts with the real clock
    cfg.totpBrowserStep = (uint32_t)(PANEL_TIME / 30) + 1;
    webNow = PANEL_TIME + 30;
    login(PANEL_TIME + 30, "");
    TEST_ASSERT_EQUAL(401, f.srv.code);
    webNow = PANEL_TIME + 60;
    login(PANEL_TIME + 60, "");
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

void test_browser_time_while_clock_not_synced() {
    Fixture f;
    f.srv.authOk = false;
    webNow = 0;
    char code[8];
    Totp::codeAt(cfg.totp, PANEL_TIME, code);
    std::string at = std::to_string((long long)PANEL_TIME);

    f.get("/login");
    TEST_ASSERT_TRUE(contains(f.out(), "<input type='hidden' name='t'>"));

    auto login = [&](const char* c, const std::string& t) {
        f.srv.request(HTTP_POST, "/login", {{"user", "admin"}, {"pass", "strong-pass"}, {"code", c},
                                            {"t", t}, {"_csrf", f.web.csrfToken()}});
    };
    // Not a time: the form explains the clock, and nothing counts
    const char* bad[] = {"", "abc", "1700100000x", "-1700100000", "1699999999", "4000000000",
                         "17001000000", " 1700100000", "170010000x"};
    for (const char* t : bad) {
        login(code, t);
        TEST_ASSERT_EQUAL(200, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "clock not synchronized"));
    }
    TEST_ASSERT_FALSE(logHas(logger, "Login: repeated failures"));

    login(otherCode(code), at);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    login(code, at);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(PANEL_TIME / 30), cfg.totpBrowserStep);   // kept apart
    login(code, at);                                   // spent
    TEST_ASSERT_EQUAL(401, f.srv.code);
    char old[8];
    Totp::codeAt(cfg.totp, PANEL_TIME - 600, old);     // an older code with its time
    login(old, std::to_string((long long)(PANEL_TIME - 600)));
    TEST_ASSERT_EQUAL(401, f.srv.code);

    // With the clock set, t is ignored: a code for t but not for the clock fails
    webNow = PANEL_TIME + 3000;
    char later[8];
    Totp::codeAt(cfg.totp, PANEL_TIME + 600, later);
    login(later, std::to_string((long long)(PANEL_TIME + 600)));
    TEST_ASSERT_EQUAL(401, f.srv.code);
    Totp::codeAt(cfg.totp, webNow, later);
    login(later, "1");
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

// In setup mode with a password, the panel can still be reached to change
// the Wi-Fi: signing in with the browser's time
void test_setup_mode_sign_in_without_clock() {
    Fixture f;
    f.enterConfigMode();
    f.srv.authOk = false;
    f.get("/");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
    char code[8];
    Totp::codeAt(cfg.totp, PANEL_TIME, code);
    f.srv.request(HTTP_POST, "/login", {{"user", "admin"}, {"pass", "strong-pass"}, {"code", code},
                                        {"t", std::to_string((long long)PANEL_TIME)},
                                        {"_csrf", f.web.csrfToken()}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.keepCookie();
    f.srv.authOk = true;
    f.get("/");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Setup mode"));
}

void test_session_expires_and_a_new_login_replaces_it() {
    Fixture f;
    std::string first = f.srv.cookie;
    setMillis(WebService::SESSION_MS - 1);
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    setMillis(WebService::SESSION_MS);
    f.get("/status");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
    f.get("/status");
    TEST_ASSERT_EQUAL(302, f.srv.code);

    f.signIn();
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    std::string second = f.srv.cookie;
    TEST_ASSERT_TRUE(first != second);

    f.signIn();
    f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", first}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", second}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

// Forms of the signed-in panel carry the session's token, not the one the
// sign-in page shows to anyone. Each sign-in draws a new one.
void test_csrf_token_is_per_session() {
    Fixture f;
    std::string boot = f.web.csrfToken();
    std::string mine = f.web.sessionCsrfToken();
    TEST_ASSERT_EQUAL(16, (int)mine.size());
    TEST_ASSERT_TRUE(boot != mine);

    f.srv.authOk = false;
    f.get("/login");
    TEST_ASSERT_TRUE(contains(f.out(), boot.c_str()));
    TEST_ASSERT_FALSE(contains(f.out(), mine.c_str()));
    f.srv.authOk = true;
    f.get("/password");
    TEST_ASSERT_TRUE(contains(f.out(), mine.c_str()));
    TEST_ASSERT_FALSE(contains(f.out(), boot.c_str()));

    f.srv.request(HTTP_POST, "/config/save", {{"name", "x"}, {"hostname", ""}, {"_csrf", boot}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    f.srv.request(HTTP_POST, "/iot/token", {{"_csrf", boot}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    f.srv.request(HTTP_POST, "/config/save", {{"name", "x"}, {"hostname", ""}, {"_csrf", mine + "0"}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    f.srv.request(HTTP_POST, "/config/save", {{"name", "x"}, {"hostname", ""}, {"_csrf", mine.substr(0, 15)}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    f.srv.request(HTTP_POST, "/config/save", {{"name", "x"}, {"hostname", ""}, {"_csrf", mine}});
    TEST_ASSERT_EQUAL(200, f.srv.code);

    // The sign-in form takes the boot token only
    webNow = PANEL_TIME;
    char code[8];
    Totp::codeAt(cfg.totp, webNow, code);
    f.srv.request(HTTP_POST, "/login", {{"user", "admin"}, {"pass", "strong-pass"}, {"code", code},
                                        {"_csrf", mine}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    f.signIn();
    TEST_ASSERT_TRUE(mine != f.web.sessionCsrfToken());
    f.srv.request(HTTP_POST, "/config/save", {{"name", "x"}, {"hostname", ""}, {"_csrf", mine}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
}

// Another device on the network sees the sign-in token, but without the
// session it cannot sign the owner out
void test_logout_without_session_changes_nothing() {
    Fixture f;
    f.srv.authOk = false;
    f.srv.request(HTTP_POST, "/logout", {{"_csrf", f.web.csrfToken()}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
    TEST_ASSERT_EQUAL(0u, f.srv.headers.count("Set-Cookie"));
    f.srv.authOk = true;
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

void test_logout() {
    Fixture f;
    std::string cookie = f.srv.cookie;
    f.srv.request(HTTP_POST, "/logout", {{"_csrf", "00000000deadbeef"}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);

    f.post("/logout");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
    TEST_ASSERT_TRUE(contains(f.srv.headers["Set-Cookie"], "Max-Age=0"));
    f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", cookie}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
}

void test_replace_authenticator() {
    Fixture f;
    for (int i = 0; i < 6; i++) {
        webNow = 0;
        f.post("/mfa/reset", {{"pass", "nope-nope!"}, {"code", "000000"}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "clock not synchronized"));
    }
    TEST_ASSERT_EQUAL_MEMORY(PANEL_TOTP, cfg.totp, sizeof(PANEL_TOTP));
    TEST_ASSERT_FALSE(logHas(logger, "Login: repeated failures"));

    webNow = PANEL_TIME;
    char good[8];
    Totp::codeAt(PANEL_TOTP, webNow, good);
    f.post("/mfa/reset", {{"pass", "wrong-pass!"}, {"code", good}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    f.post("/mfa/reset", {{"pass", "strong-pass"}, {"code", otherCode(good)}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_MEMORY(PANEL_TOTP, cfg.totp, sizeof(PANEL_TOTP));

    std::string fullCookie = f.srv.cookie;
    f.post("/mfa/reset", {{"pass", "strong-pass"}, {"code", good}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/mfa", f.srv.headers["Location"].c_str());
    TEST_ASSERT_TRUE(logHas(logger, "Panel: authenticator reset"));
    // Nothing changes until a code from the new key: the current one stays
    TEST_ASSERT_EQUAL_MEMORY(PANEL_TOTP, cfg.totp, sizeof(PANEL_TOTP));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(PANEL_TIME / 30), cfg.totpStep);
    // A new cookie for the setup level; the signed-in one is gone
    f.keepCookie();
    TEST_ASSERT_TRUE(fullCookie != f.srv.cookie);
    f.srv.request(HTTP_GET, "/status", {}, {{"Cookie", fullCookie}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());

    f.post("/mfa/reset", {{"pass", "strong-pass"}, {"code", good}});   // spent, and not signed in
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/mfa", f.srv.headers["Location"].c_str());
    f.get("/status");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/mfa", f.srv.headers["Location"].c_str());
    f.get("/login");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/mfa", f.srv.headers["Location"].c_str());
    f.get("/mfa");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Sign out"));          // a way to give up
    uint8_t key[Totp::KEY_LEN];
    TEST_ASSERT_TRUE(secretFrom(f.out(), key));
    TEST_ASSERT_TRUE(std::memcmp(key, PANEL_TOTP, Totp::KEY_LEN) != 0);

    char code[8];
    webNow = PANEL_TIME + 30;
    Totp::codeAt(key, webNow, code);
    MockFS::failWriteOpen = true;
    f.post("/mfa", {{"code", code}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Could not save"));
    TEST_ASSERT_EQUAL_MEMORY(PANEL_TOTP, cfg.totp, sizeof(PANEL_TOTP));   // not zeros
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(PANEL_TIME / 30), cfg.totpStep);
    MockFS::failWriteOpen = false;

    f.post("/mfa", {{"code", code}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());
    TEST_ASSERT_EQUAL_MEMORY(key, cfg.totp, Totp::KEY_LEN);
    f.keepCookie();
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);

    // The old key no longer signs in; the new one does
    f.srv.authOk = false;
    char oldCode[8];
    webNow = PANEL_TIME + 90;
    Totp::codeAt(PANEL_TOTP, webNow, oldCode);
    postLogin(f, "admin", "strong-pass", oldCode);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    Totp::codeAt(key, webNow, code);
    postLogin(f, "admin", "strong-pass", code);
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

// Giving up a replacement (signing out, the session ending, a reboot) keeps
// the current key: the password alone never opens enrollment again.
void test_replacement_given_up_keeps_key() {
    Fixture f;
    webNow = PANEL_TIME;
    char good[8];
    Totp::codeAt(PANEL_TOTP, webNow, good);
    f.post("/mfa/reset", {{"pass", "strong-pass"}, {"code", good}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.keepCookie();
    f.post("/logout");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_MEMORY(PANEL_TOTP, cfg.totp, sizeof(PANEL_TOTP));

    f.srv.authOk = false;
    f.get("/login");
    TEST_ASSERT_TRUE(contains(f.out(), "name='code'"));
    postLogin(f, "admin", "strong-pass", nullptr);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    webNow = PANEL_TIME + 30;
    Totp::codeAt(PANEL_TOTP, webNow, good);
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/", f.srv.headers["Location"].c_str());

    // Session ending in the middle of a replacement
    f.keepCookie();
    f.srv.authOk = true;
    webNow = PANEL_TIME + 60;
    Totp::codeAt(PANEL_TOTP, webNow, good);
    f.post("/mfa/reset", {{"pass", "strong-pass"}, {"code", good}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
    f.keepCookie();
    setMillis(WebService::SESSION_MS);
    f.get("/mfa");
    TEST_ASSERT_EQUAL(302, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/login", f.srv.headers["Location"].c_str());
    TEST_ASSERT_EQUAL_MEMORY(PANEL_TOTP, cfg.totp, sizeof(PANEL_TOTP));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "31323334353637383930"));
}

void test_login_compares_full_length() {
    Fixture f;
    f.srv.authOk = false;
    webNow = PANEL_TIME;
    char good[8];
    Totp::codeAt(PANEL_TOTP, webNow, good);
    postLogin(f, "admin", "strong-pass", "12345a");
    TEST_ASSERT_EQUAL(401, f.srv.code);
    postLogin(f, "admin", "strong-pass", "1234567");
    TEST_ASSERT_EQUAL(401, f.srv.code);
    std::string longUser(33, 'u');
    postLogin(f, longUser.c_str(), "strong-pass", good);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    postLogin(f, "admin", "strong-paxs", good);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    postLogin(f, " admin ", "strong-pass", good);
    TEST_ASSERT_EQUAL(302, f.srv.code);

    std::string tooLong(65, 'p');
    postLogin(f, "admin", tooLong.c_str(), good);
    TEST_ASSERT_EQUAL(401, f.srv.code);
    std::string atCap(64, 'p');
    postLogin(f, "admin", atCap.c_str(), good);
    TEST_ASSERT_EQUAL(401, f.srv.code);

    cfg.webusr = std::string(33, 'A').c_str();
    postLogin(f, "admin", "strong-pass", good);
    TEST_ASSERT_EQUAL(401, f.srv.code);
}

void test_enroll_post_rejects_bad_csrf() {
    std::memset(cfg.totp, 0, sizeof(cfg.totp));
    Fixture f;
    f.srv.authOk = false;
    webNow = PANEL_TIME;
    postLogin(f, "admin", "strong-pass", nullptr);
    f.keepCookie();
    f.srv.authOk = true;
    f.srv.request(HTTP_POST, "/mfa", {{"_csrf", "00000000deadbeef"}, {"code", "000000"}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    TEST_ASSERT_TRUE(Totp::blank(cfg.totp));
    TEST_ASSERT_FALSE(logHas(logger, "Panel: authenticator on"));
}

void test_unix_time() {
    IoTService bare{cfg, logger};
    TEST_ASSERT_EQUAL_INT(0, (int)bare.unixTime());
    Clock clock{webTime};
    IoTService timed{cfg, logger, &clock};
    webNow = 0;
    TEST_ASSERT_EQUAL_INT(0, (int)timed.unixTime());
    webNow = Clock::MIN_VALID_EPOCH + 5;
    TEST_ASSERT_EQUAL_INT((int)(Clock::MIN_VALID_EPOCH + 5), (int)timed.unixTime());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_escape);
    RUN_TEST(test_mask_secret);
    RUN_TEST(test_format_duration);
    RUN_TEST(test_format_bytes);
    RUN_TEST(test_begin_opens_port_and_makes_random_csrf);
    RUN_TEST(test_tick_serves_clients);
    RUN_TEST(test_unknown_route);
    RUN_TEST(test_without_password_redirects_to_set_password);
    RUN_TEST(test_authentication_required);
    RUN_TEST(test_session_authentication);
    RUN_TEST(test_code_works_once);
    RUN_TEST(test_browser_clock_ahead_does_not_lock_out);
    RUN_TEST(test_browser_time_while_clock_not_synced);
    RUN_TEST(test_setup_mode_sign_in_without_clock);
    RUN_TEST(test_replacement_given_up_keeps_key);
    RUN_TEST(test_password_save_needs_current_password_and_code);
    RUN_TEST(test_csrf_token_is_per_session);
    RUN_TEST(test_logout_without_session_changes_nothing);
    RUN_TEST(test_authenticator_is_required);
    RUN_TEST(test_login_pages_without_password_go_to_setup);
    RUN_TEST(test_enroll_lockout);
    RUN_TEST(test_code_window);
    RUN_TEST(test_session_expires_and_a_new_login_replaces_it);
    RUN_TEST(test_logout);
    RUN_TEST(test_replace_authenticator);
    RUN_TEST(test_login_compares_full_length);
    RUN_TEST(test_enroll_post_rejects_bad_csrf);
    RUN_TEST(test_unix_time);
    RUN_TEST(test_invalid_csrf_rejected_on_every_post);
    RUN_TEST(test_root_station_mode);
    RUN_TEST(test_root_with_ip_and_last_update);
    RUN_TEST(test_root_shows_ota_progress);
    RUN_TEST(test_root_setup_mode_first_access);
    RUN_TEST(test_root_setup_mode_keeps_user);
    RUN_TEST(test_root_setup_mode_with_password_set);
    RUN_TEST(test_save_wifi_ok);
    RUN_TEST(test_save_wifi_open_network);
    RUN_TEST(test_save_wifi_validation);
    RUN_TEST(test_save_wifi_first_access_needs_panel_password);
    RUN_TEST(test_save_wifi_write_fails);
    RUN_TEST(test_device_photo_is_avif);
    RUN_TEST(test_gzipped_css_and_js);
    RUN_TEST(test_embedded_gzipped_svg_pinout);
    RUN_TEST(test_log_escapes_content);
    RUN_TEST(test_status_connected);
    RUN_TEST(test_status_disconnected_and_ota);
    RUN_TEST(test_status_public_ip);
    RUN_TEST(test_cloudflare_without_token);
    RUN_TEST(test_cloudflare_with_masked_token_and_escaped_values);
    RUN_TEST(test_name_in_title_and_header);
    RUN_TEST(test_settings_save_name_and_hostname);
    RUN_TEST(test_unknown_host_is_refused);
    RUN_TEST(test_settings_save_other_addresses);
    RUN_TEST(test_settings_refuse_invalid_name_or_hostname);
    RUN_TEST(test_time_zone);
    RUN_TEST(test_panel_language);
    RUN_TEST(test_panel_language_escaped);
    RUN_TEST(test_local_time_on_status);
    RUN_TEST(test_iot_time_window);
    RUN_TEST(test_iot_sht3x_uses_two_pins);
    RUN_TEST(test_power_in_header);
    RUN_TEST(test_settings_save_gauge);
    RUN_TEST(test_iot_respects_gauge);
    RUN_TEST(test_cloudflare_save_ok);
    RUN_TEST(test_cloudflare_save_empty_token_keeps_current);
    RUN_TEST(test_cloudflare_save_rejects_odd_ids_and_token);
    RUN_TEST(test_cloudflare_save_long_value_rejected_unchanged);
    RUN_TEST(test_cloudflare_save_write_fails);
    RUN_TEST(test_ntfy_page);
    RUN_TEST(test_ntfy_save);
    RUN_TEST(test_ntfy_save_write_fails);
    RUN_TEST(test_password_page_first_access_without_auth);
    RUN_TEST(test_password_page_with_password);
    RUN_TEST(test_password_save_rotates_csrf);
    RUN_TEST(test_password_save_first_password);
    RUN_TEST(test_blank_key_code_never_matches);
    RUN_TEST(test_menu_only_where_it_opens);
    RUN_TEST(test_menu_on_first_setup);
    RUN_TEST(test_password_save_invalid);
    RUN_TEST(test_password_save_write_fails);
    RUN_TEST(test_update_initial_page);
    RUN_TEST(test_update_automatic_off);
    RUN_TEST(test_update_saves_automatic);
    RUN_TEST(test_update_auto_write_fails);
    RUN_TEST(test_update_check_redirects_and_shows_result);
    RUN_TEST(test_update_install_without_version);
    RUN_TEST(test_update_install_shows_progress);
    RUN_TEST(test_iot_page_with_nothing_set);
    RUN_TEST(test_iot_page_sensor_and_actuator_with_live_reading);
    RUN_TEST(test_iot_page_sensor_only_and_actuator_only);
    RUN_TEST(test_iot_position_and_color_controls);
    RUN_TEST(test_iot_unknown_type_warns);
    RUN_TEST(test_iot_now_as_json);
    RUN_TEST(test_iot_cmd_from_panel);
    RUN_TEST(test_iot_save_digital_sensor_and_actuator);
    RUN_TEST(test_iot_save_numeric_with_proportional_level);
    RUN_TEST(test_iot_save_position_color_and_roles);
    RUN_TEST(test_iot_save_rejects_values);
    RUN_TEST(test_iot_save_write_fails);
    RUN_TEST(test_api_exists_only_with_iot_set);
    RUN_TEST(test_api_requires_token);
    RUN_TEST(test_api_reads_and_commands);
    RUN_TEST(test_api_token_generated_shown_and_replaced);
    RUN_TEST(test_api_without_token_and_iot_off);
    RUN_TEST(test_ntfy_commands_page_and_validation);
    RUN_TEST(test_ntfy_page_without_command_service);
    RUN_TEST(test_iot_remove_asks_for_confirmation);
    RUN_TEST(test_iot_remove_write_fails);
    RUN_TEST(test_mqtt_page);
    RUN_TEST(test_mqtt_page_without_service);
    RUN_TEST(test_mqtt_save);
    RUN_TEST(test_mqtt_save_validation);
    RUN_TEST(test_status_shows_ap_password);
    RUN_TEST(test_login_repeated_failures_wait_longer);
    RUN_TEST(test_login_wait_has_cap_and_request_without_credentials_does_not_count);
    RUN_TEST(test_wrong_api_token_counts_as_failure);
    RUN_TEST(test_guessing_elsewhere_does_not_lock_the_owner_out);
    RUN_TEST(test_ntfy_short_command_topic_without_token);
    RUN_TEST(test_ntfy_warns_short_topic_already_saved);
    RUN_TEST(test_security_headers);
    RUN_TEST(test_log_says_where_the_command_came_from);
    return UNITY_END();
}
