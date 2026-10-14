// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "App.h"
#include "../web_browser.h"

void setUp()    { resetMocks(); }
void tearDown() {}

void test_begin_starts_services() {
    MockFS::put("/config.json", R"({"ssid":"Home","pass":"12345678","webpss":"strong-pass"})");
    App app;
    app.begin();

    TEST_ASSERT_EQUAL(115200, Serial.baud);
    TEST_ASSERT_EQUAL_STRING("Home", app.config.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Home", WiFi.beginSSID.c_str());
    TEST_ASSERT_NOT_NULL(WiFi.gotIP);
    TEST_ASSERT_EQUAL(1, mock::configTimeCalls);
    TEST_ASSERT_TRUE(MockServer::begun);
    TEST_ASSERT_TRUE(logHas(app.logger, "System started dev"));
    TEST_ASSERT_TRUE(logHas(app.logger, "Reset: Power On"));
    TEST_ASSERT_TRUE(logHas(app.logger, "OTA: reserve 400 KB"));
    TEST_ASSERT_TRUE(app.ota.automatic());
}

// Battery gauge set up and not answering: tick reads and reports it
void test_tick_reads_battery_gauge() {
    MockFS::put("/config.json", R"({"ssid":"Home","pass":"12345678","webpss":"strong-pass","bat_sda":3})");
    App app;
    app.begin();
    TEST_ASSERT_TRUE(app.power.configured());
    app.tick();
    TEST_ASSERT_TRUE(logHas(app.logger, "Power: gauge not answering"));
}

void test_begin_turns_automatic_ota_off() {
    MockFS::put("/config.json", R"({"ssid":"Home","pass":"12345678","webpss":"strong-pass","ota_auto":false})");
    App app;
    app.begin();
    TEST_ASSERT_FALSE(app.config.ota_auto);
    TEST_ASSERT_FALSE(app.ota.automatic());
}

void test_begin_without_config_enters_ap_mode() {
    App app;
    app.begin();
    TEST_ASSERT_TRUE(app.wifi.inConfigMode());
    TEST_ASSERT_EQUAL(WIFI_AP, WiFi._mode);
}

void test_tick_runs_every_service() {
    App app;
    app.begin();
    setMillis(WiFiService::DAILY_RESTART + 1);
    MockBrowser b{[&]() { app.tick(); }};
    b.request(HTTP_GET, "/status");
    TEST_ASSERT_EQUAL(200, b.code);
    TEST_ASSERT_TRUE(app.wifi.restartPending());
}

void test_begin_ota_recovery() {
    MockFS::put("/config.json", R"({"ssid":"Home","pass":"12345678","webpss":"strong-pass"})");
    MockFS::put("/ota.pending", "v129 1");
    App app;
    app.begin();
    TEST_ASSERT_TRUE(app.ota.slim());
    TEST_ASSERT_TRUE(app.ota.busy());
    TEST_ASSERT_TRUE(logHas(app.logger, "OTA: recovery mode"));
    TEST_ASSERT_EQUAL_STRING("none", app.iot.sensorText());
}

// Recovery: only Wi-Fi and OTA run; panel, DNS, IoT and ntfy stay idle
void test_tick_in_recovery_runs_only_ota() {
    MockFS::put("/config.json", R"({"ssid":"Home","pass":"12345678","webpss":"strong-pass"})");
    MockFS::put("/ota.pending", "v129 1");
    App app;
    app.begin();
    MockHTTP::push(200, std::string(512, 'X'), 4096);
    MockBrowser b{[&]() { app.tick(); }};
    b.request(HTTP_GET, "/status");
    TEST_ASSERT_TRUE(app.ota.inProgress());
    TEST_ASSERT_EQUAL(0, b.code);   // panel idle: nobody answers
    TEST_ASSERT_FALSE(MockServer::begun);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs[0], "/download/v129/"));
}

void test_tick_pauses_dns_during_ota() {
    MockFS::put("/config.json", R"({"ssid":"Home","pass":"12345678","webpss":"strong-pass"})");
    App app;
    app.begin();
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(512, 'X'), 4096);
    app.ota.requestCheck();
    app.tick();
    app.ota.requestInstall();
    app.tick();
    TEST_ASSERT_TRUE(app.ota.inProgress());
    size_t before = MockHTTP::calledURLs.size();
    setMillis(120000);
    app.tick();
    TEST_ASSERT_EQUAL((int)before, (int)MockHTTP::calledURLs.size());
}

// The IP check and DNS use TLS: the ntfy command stream releases its session first
void test_tick_releases_ntfy_before_ip_and_dns() {
    MockFS::put("/config.json", R"({"v":2,"ssid":"Home","pass":"12345678",
        "webpss":"strong-pass","ota_auto":false,"cf_token":"T","cf_zone":"Z",
        "cf_records":["R"],"ntfy_cmd":"cmd-x","actuator":"relay","actuator_pin":0})");
    App app;
    app.begin();
    TEST_ASSERT_FALSE(app.ota.wantsNet(1000));
    setMillis(1000);
    app.tick();                                    // public IP first
    TEST_ASSERT_EQUAL_STRING("paused", app.ntfy.commandStatus());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs[0], "ipify"));

    setMillis(2000);                               // failed: tries in 1 min
    app.tick();
    TEST_ASSERT_EQUAL_STRING("connecting", app.ntfy.commandStatus());

    strlcpy(app.config.publicIP, "1.2.3.4", sizeof(app.config.publicIP));
    setMillis(3000);                               // IP known: DNS
    app.tick();
    TEST_ASSERT_EQUAL_STRING("paused", app.ntfy.commandStatus());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "/dns_records/R"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_starts_services);
    RUN_TEST(test_tick_reads_battery_gauge);
    RUN_TEST(test_begin_turns_automatic_ota_off);
    RUN_TEST(test_begin_without_config_enters_ap_mode);
    RUN_TEST(test_tick_runs_every_service);
    RUN_TEST(test_begin_ota_recovery);
    RUN_TEST(test_tick_in_recovery_runs_only_ota);
    RUN_TEST(test_tick_pauses_dns_during_ota);
    RUN_TEST(test_tick_releases_ntfy_before_ip_and_dns);
    return UNITY_END();
}
