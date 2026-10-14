// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "OTAService.h"
#include "WebService.h"
#include "../web_browser.h"
#include "../i2c_sim.h"

using Args = std::map<std::string, std::string>;

static Logger logger;
static Config cfg;
static time_t webNow = 0;
static time_t webTime() { return webNow; }

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
              withMqtt ? &mqtt : nullptr, withPower ? &power : nullptr) { web.begin(); }

    bool get(const char* path)              { return srv.request(HTTP_GET, path); }
    bool post(const char* path, Args a = {}) {
        a["_csrf"] = web.csrfToken();
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

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    cfg.webusr = "admin";
    cfg.webpss = "strong-pass";
    cfg.ssid   = "Home";
    webNow = 0;
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
    const char* paths[] = {"/", "/status", "/log", "/cloudflare", "/ntfy", "/mqtt", "/password", "/update", "/iot", "/config"};
    for (const char* p : paths) {
        f.get(p);
        TEST_ASSERT_EQUAL(401, f.srv.code);
        TEST_ASSERT_EQUAL_STRING("Basic realm=\"ESP8266\"", f.srv.headers["WWW-Authenticate"].c_str());
    }
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/mqtt/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install", "/iot/save", "/iot/cmd", "/iot/token", "/iot/clear", "/config/save"};
    for (const char* p : posts) {
        f.post(p);
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
}

// Basic only, with the panel user and password, compared in full
void test_basic_authentication() {
    Fixture f;
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);

    f.srv.basicPass = "senha-errada";
    f.get("/status");
    TEST_ASSERT_EQUAL(401, f.srv.code);
    f.srv.basicPass = "strong-pass";
    f.srv.basicUser = "other";
    f.get("/status");
    TEST_ASSERT_EQUAL(401, f.srv.code);

    // admin:strong-pass
    const char* ok = "Basic YWRtaW46c3Ryb25nLXBhc3M=";
    const char* bad[] = {
        "Basic YWRtaW46c3Ryb25nLXBhc3M",              // cut
        "Basic YWRtaW46c3Ryb25nLXBhc3M==",            // extra
        "basic YWRtaW46c3Ryb25nLXBhc3M=",             // another scheme
        "Digest username=\"admin\", realm=\"ESP8266\"",
        "Bearer YWRtaW46c3Ryb25nLXBhc3M=",
        "",
    };
    f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", ok}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    for (const char* auth : bad) {
        f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", auth}});
        TEST_ASSERT_EQUAL(401, f.srv.code);
        f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", ok}});   // resets the failure count
    }
}

void test_invalid_csrf_rejected_on_every_post() {
    Fixture f;
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/mqtt/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install", "/iot/save", "/iot/cmd", "/iot/token", "/iot/clear", "/config/save"};
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
    TEST_ASSERT_TRUE(contains(f.out(), "Firmware: dev"));
    TEST_ASSERT_FALSE(contains(f.out(), "OTA in progress"));
    TEST_ASSERT_TRUE(contains(f.out(), "/esp01.webp"));
    TEST_ASSERT_TRUE(contains(f.out(), "alt='ESP-01'"));
    TEST_ASSERT_TRUE(contains(f.out(), "Firmware for the ESP-01 module"));
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
    TEST_ASSERT_TRUE(contains(f.out(), "/esp01.webp"));
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
    TEST_ASSERT_FALSE(contains(f.out(), "<script src"));
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
    TEST_ASSERT_EQUAL_STRING("abcdefgh", cfg.webpss.c_str());
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

void test_embedded_device_image() {
    cfg.webpss = "";   // public: loads before there is a password
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/esp01.webp"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("image/webp", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL(sizeof(ASSET_PHOTO), f.out().size());
    TEST_ASSERT_EQUAL(0, f.out().compare(0, 4, "RIFF"));
    TEST_ASSERT_EQUAL(0, f.out().compare(8, 4, "WEBP"));
    TEST_ASSERT_EQUAL_STRING("max-age=604800", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL(0u, f.srv.headers.count("Content-Encoding"));
}

// Panel CSS and JS: gzipped, public (the first setup page uses them)
static void assetGz(const char* path, const char* type, size_t len) {
    cfg.webpss = "";
    Fixture f;
    TEST_ASSERT_TRUE(f.get(path));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING(type, f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("gzip", f.srv.headers["Content-Encoding"].c_str());
    TEST_ASSERT_EQUAL_STRING("max-age=604800", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL(len, f.out().size());
    TEST_ASSERT_EQUAL_HEX8(0x1f, (uint8_t)f.out()[0]);
    TEST_ASSERT_EQUAL_HEX8(0x8b, (uint8_t)f.out()[1]);
}

void test_gzipped_css_and_js() {
    assetGz("/s.css", "text/css", sizeof(WEB_CSS_GZ));
    assetGz("/iot.js", "text/javascript", sizeof(WEB_IOT_GZ));
    TEST_ASSERT_EQUAL(8, (int)std::strlen(WEB_CSS_TAG));
    TEST_ASSERT_EQUAL(8, (int)std::strlen(WEB_IOT_TAG));
}

void test_embedded_gzipped_svg_pinout() {
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/pinout.svg"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("gzip", f.srv.headers["Content-Encoding"].c_str());
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
    TEST_ASSERT_TRUE(contains(f.out(), "<a href='/config'>Settings</a>"));
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
    TEST_ASSERT_EQUAL(401, f.srv.code);
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

void test_cloudflare_save_ok() {
    Fixture f;
    f.post("/cloudflare/save", {
        {"token", "NOVO_TOKEN"}, {"zone", " ZONA "}, {"host", "h.com"}, {"record", " R1 "}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("NOVO_TOKEN", cfg.cf_token);
    TEST_ASSERT_EQUAL_STRING("ZONA", cfg.cf_zone);
    TEST_ASSERT_EQUAL_STRING("h.com", cfg.cf_host);
    TEST_ASSERT_EQUAL_STRING("R1", cfg.cf_record);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"cf_record\":\"R1\""));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "NOVO_TOKEN"));
}

void test_cloudflare_save_empty_token_keeps_current() {
    Fixture f;
    strlcpy(cfg.cf_token, "OLD", sizeof(cfg.cf_token));
    f.post("/cloudflare/save", {{"zone", "Z"}, {"record", "R"}});
    TEST_ASSERT_EQUAL_STRING("OLD", cfg.cf_token);
}

void test_cloudflare_save_long_value_rejected_unchanged() {
    Fixture f;
    strlcpy(cfg.cf_zone, "ORIGINAL", sizeof(cfg.cf_zone));
    f.post("/cloudflare/save", {{"zone", "Z"}, {"record", std::string(40, 'r')}});
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
    f.post("/cloudflare/save", {{"zone", "Z"}});
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
    TEST_ASSERT_TRUE(contains(f.out(), "href='/mqtt'"));

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
}

void test_password_page_with_password() {
    Fixture f;
    f.get("/password");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "Set a password"));
}

void test_password_save_rotates_csrf() {
    Fixture f;
    std::string old = f.web.csrfToken();
    f.post("/password/save", {{"user", "new"}, {"pass1", "secret123"}, {"pass2", "secret123"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("new",       cfg.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("secret123", cfg.webpss.c_str());
    TEST_ASSERT_TRUE(old != f.web.csrfToken());
}

void test_password_save_invalid() {
    Fixture f;
    f.post("/password/save", {{"user", "u"}, {"pass1", "secret123"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    f.post("/password/save", {{"user", std::string(33, 'u')}, {"pass1", "secret123"}, {"pass2", "secret123"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    f.post("/password/save", {{"user", "u"}, {"pass1", std::string(65, 'p')}, {"pass2", std::string(65, 'p')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("strong-pass", cfg.webpss.c_str());
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
    f.post("/password/save", {{"user", "u"}, {"pass1", "secret123"}, {"pass2", "secret123"}});
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
    TEST_ASSERT_TRUE(contains(f.out(), "/pinout.svg"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='n' checked>Nothing"));
    TEST_ASSERT_TRUE(contains(f.out(), "<legend>What does this device do?</legend>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<optgroup label='Temperature and humidity'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<optgroup label='Color'><option value='strip'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='-1' selected>Choose</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "var SK={dht22:3,dht11:2"));
    TEST_ASSERT_TRUE(contains(f.out(), "strip:4},T='"));
    TEST_ASSERT_TRUE(contains(f.out(), f.web.csrfToken()));
    TEST_ASSERT_TRUE(contains(f.out(), "';</script><script src='/iot.js?v=" WEB_IOT_TAG "'></script>"));
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
    TEST_ASSERT_EQUAL(401, f.srv.code);
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
    TEST_ASSERT_EQUAL(401, f.srv.code);
}

// ── Hardening for remote access ──────────────────────────────────────────────

static const char* OK_BASIC  = "Basic YWRtaW46c3Ryb25nLXBhc3M=";   // admin:strong-pass
static const char* BAD_BASIC = "Basic YWRtaW46d3Jvbmc=";           // admin:wrong

static int statusWith(Fixture& f, const char* auth) {
    f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", auth}});
    return f.srv.code;
}

void test_login_repeated_failures_wait_longer() {
    Fixture f;
    f.srv.authOk = false;
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) TEST_ASSERT_EQUAL(401, statusWith(f, BAD_BASIC));
    TEST_ASSERT_TRUE(logHas(logger, "Login: repeated failures"));

    // While waiting, not even the right password is tried
    TEST_ASSERT_EQUAL(429, statusWith(f, OK_BASIC));
    TEST_ASSERT_TRUE(contains(f.out(), "Too many attempts"));
    setMillis(WebService::LOGIN_WAIT_MS - 1);
    TEST_ASSERT_EQUAL(429, statusWith(f, BAD_BASIC));
    setMillis(WebService::LOGIN_WAIT_MS);
    TEST_ASSERT_EQUAL(401, statusWith(f, BAD_BASIC));          // 6th failure: the wait doubles
    unsigned long t = WebService::LOGIN_WAIT_MS;
    setMillis(t + 2 * WebService::LOGIN_WAIT_MS - 1);
    TEST_ASSERT_EQUAL(429, statusWith(f, OK_BASIC));
    setMillis(t + 2 * WebService::LOGIN_WAIT_MS);
    TEST_ASSERT_EQUAL(200, statusWith(f, OK_BASIC));           // getting it right resets the count

    for (int i = 0; i < WebService::FREE_LOGIN_FAILS - 1; i++) TEST_ASSERT_EQUAL(401, statusWith(f, BAD_BASIC));
    TEST_ASSERT_EQUAL(200, statusWith(f, OK_BASIC));
}

void test_login_wait_has_cap_and_request_without_credentials_does_not_count() {
    Fixture f;
    f.srv.authOk = false;
    // The browser's first request comes without credentials: it does not count
    for (int i = 0; i < 20; i++) TEST_ASSERT_EQUAL(401, statusWith(f, ""));
    TEST_ASSERT_FALSE(logHas(logger, "Login: repeated failures"));

    unsigned long t = 0;
    for (int i = 0; i < 260; i++) {                           // until the counter stops at 255
        statusWith(f, BAD_BASIC);
        t += WebService::LOGIN_WAIT_MAX_MS;                       // always after the wait
        setMillis(t);
    }
    statusWith(f, BAD_BASIC);
    setMillis(t + WebService::LOGIN_WAIT_MAX_MS - 1);
    TEST_ASSERT_EQUAL(429, statusWith(f, OK_BASIC));
    setMillis(t + WebService::LOGIN_WAIT_MAX_MS);
    TEST_ASSERT_EQUAL(200, statusWith(f, OK_BASIC));           // never more than 5 min
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
    RUN_TEST(test_basic_authentication);
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
    RUN_TEST(test_embedded_device_image);
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
    RUN_TEST(test_settings_refuse_invalid_name_or_hostname);
    RUN_TEST(test_time_zone);
    RUN_TEST(test_local_time_on_status);
    RUN_TEST(test_iot_time_window);
    RUN_TEST(test_iot_sht3x_uses_two_pins);
    RUN_TEST(test_power_in_header);
    RUN_TEST(test_settings_save_gauge);
    RUN_TEST(test_iot_respects_gauge);
    RUN_TEST(test_cloudflare_save_ok);
    RUN_TEST(test_cloudflare_save_empty_token_keeps_current);
    RUN_TEST(test_cloudflare_save_long_value_rejected_unchanged);
    RUN_TEST(test_cloudflare_save_write_fails);
    RUN_TEST(test_ntfy_page);
    RUN_TEST(test_ntfy_save);
    RUN_TEST(test_ntfy_save_write_fails);
    RUN_TEST(test_password_page_first_access_without_auth);
    RUN_TEST(test_password_page_with_password);
    RUN_TEST(test_password_save_rotates_csrf);
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
    RUN_TEST(test_ntfy_short_command_topic_without_token);
    RUN_TEST(test_ntfy_warns_short_topic_already_saved);
    RUN_TEST(test_security_headers);
    RUN_TEST(test_log_says_where_the_command_came_from);
    return UNITY_END();
}
