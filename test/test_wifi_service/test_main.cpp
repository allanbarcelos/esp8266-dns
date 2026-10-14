// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "WiFiService.h"

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    cfg.ssid = "Home";
    cfg.pass = "secret123";
}
void tearDown() {}

using W = WiFiService;

// ── begin() ──────────────────────────────────────────────────────────────────

void test_without_ssid_starts_protected_ap() {
    cfg.ssid = "";
    W wifi(cfg, logger);
    wifi.begin();
    TEST_ASSERT_EQUAL(WIFI_AP, WiFi._mode);
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_EQUAL_STRING(AP_SSID, WiFi.apSSID.c_str());
    TEST_ASSERT_EQUAL_STRING(wifi.apPassword(), WiFi.apPass.c_str());
    TEST_ASSERT_TRUE(std::strlen(wifi.apPassword()) >= 8);
    TEST_ASSERT_TRUE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);
}

void test_connects_in_station_mode() {
    W wifi(cfg, logger);
    wifi.begin();
    TEST_ASSERT_EQUAL(WIFI_STA, WiFi._mode);
    TEST_ASSERT_FALSE(WiFi.persistentFlag);
    TEST_ASSERT_TRUE(WiFi.autoReconnectFlag);
    TEST_ASSERT_EQUAL_STRING("Home", WiFi.beginSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("secret123", WiFi.beginPass.c_str());
    TEST_ASSERT_FALSE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_FALSE(logHas(logger, "Connected!")); // tick follows the association
    wifi.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "Connected!"));
}

// DHCP hostname: only when set, before connecting
void test_hostname_before_connecting() {
    {
        W wifi(cfg, logger);
        wifi.begin();
        TEST_ASSERT_EQUAL_STRING("", WiFi.hostName.c_str());
    }
    strlcpy(cfg.hostname, "gate", sizeof(cfg.hostname));
    W wifi(cfg, logger);
    wifi.begin();
    TEST_ASSERT_EQUAL_STRING("gate", WiFi.hostName.c_str());
}

// begin() does not wait for the association: the loop keeps going while Wi-Fi connects
void test_begin_does_not_block() {
    WiFi._status = WL_DISCONNECTED;
    W wifi(cfg, logger);
    setMillis(5000);
    wifi.begin();
    TEST_ASSERT_EQUAL(5000UL, millis());
    wifi.tick(5000 + W::CONNECT_TIMEOUT - 1);
    TEST_ASSERT_FALSE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL(WIFI_STA, WiFi._mode);
    WiFi._status = WL_CONNECTED;
    wifi.tick(5000 + W::CONNECT_TIMEOUT);
    TEST_ASSERT_FALSE(wifi.inConfigMode());
    TEST_ASSERT_TRUE(logHas(logger, "Connected!"));
    // connected: tick goes back to watching the association as usual
    WiFi._status = WL_DISCONNECTED;
    wifi.tick(5000 + W::CONNECT_TIMEOUT + W::STATUS_INTERVAL);
    TEST_ASSERT_EQUAL(1, WiFi.disconnectCalls);
}

void test_failure_starts_ap_and_station() {
    WiFi._status = WL_DISCONNECTED;
    W wifi(cfg, logger);
    wifi.begin();
    wifi.tick(W::CONNECT_TIMEOUT);
    TEST_ASSERT_EQUAL(WIFI_AP_STA, WiFi._mode);
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_TRUE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL_STRING(AP_SSID, WiFi.apSSID.c_str());
    TEST_ASSERT_TRUE(logHas(logger, "Connection failed"));
}

// ── events ───────────────────────────────────────────────────────────────────

void test_got_ip_leaves_setup_mode() {
    WiFi._status = WL_DISCONNECTED;
    W wifi(cfg, logger);
    wifi.registerEvents();
    wifi.begin();
    wifi.tick(W::CONNECT_TIMEOUT);
    TEST_ASSERT_TRUE(wifi.inConfigMode());

    WiFi._status = WL_CONNECTED;
    WiFi.gotIP(WiFiEventStationModeGotIP{});
    TEST_ASSERT_FALSE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL(WIFI_STA, WiFi._mode);
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_TRUE(logHas(logger, "AP mode off"));
}

void test_got_ip_in_station_mode_keeps_mode() {
    W wifi(cfg, logger);
    wifi.registerEvents();
    wifi.begin();
    WiFi._mode = WIFI_STA;
    WiFi.gotIP(WiFiEventStationModeGotIP{});
    TEST_ASSERT_FALSE(logHas(logger, "AP mode off"));
}

void test_got_ip_resets_failures() {
    W wifi(cfg, logger);
    wifi.registerEvents();
    for (int i = 0; i < W::MAX_FAILURES - 1; i++) wifi.notifyFailure();
    WiFi.gotIP(WiFiEventStationModeGotIP{});
    wifi.notifyFailure();
    TEST_ASSERT_FALSE(wifi.restartPending());
}

void test_disconnected_event_logs() {
    W wifi(cfg, logger);
    wifi.registerEvents();
    WiFi.disconnected(WiFiEventStationModeDisconnected{});
    TEST_ASSERT_TRUE(logHas(logger, "Wi-Fi disconnected"));
}

// ── tick() ───────────────────────────────────────────────────────────────────

void test_setup_mode_keeps_ap() {
    cfg.ssid = "";
    W wifi(cfg, logger);
    wifi.begin();
    WiFi._status = WL_DISCONNECTED;

    for (unsigned long t = 0; t <= 10 * W::STATUS_INTERVAL; t += 500) {
        wifi.tick(t);
        wifi.tickRestart(t);
    }
    TEST_ASSERT_EQUAL(WIFI_AP, WiFi._mode);
    TEST_ASSERT_EQUAL(0, WiFi.disconnectCalls);
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);
    TEST_ASSERT_FALSE(wifi.restartPending());
}

void test_reconnects_when_disconnected() {
    W wifi(cfg, logger);
    WiFi._status = WL_DISCONNECTED;

    wifi.tick(W::STATUS_INTERVAL);
    TEST_ASSERT_EQUAL(1, WiFi.disconnectCalls);
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);

    wifi.tick(W::STATUS_INTERVAL + W::RECONNECT_DELAY - 1);
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);

    wifi.tick(W::STATUS_INTERVAL + W::RECONNECT_DELAY);
    TEST_ASSERT_EQUAL(1, WiFi.beginCalls);
    TEST_ASSERT_EQUAL(WIFI_STA, WiFi._mode);
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_TRUE(logHas(logger, "Trying to reconnect"));
}

void test_does_not_start_duplicate_reconnection() {
    W wifi(cfg, logger);
    WiFi._status = WL_DISCONNECTED;
    wifi.tick(W::STATUS_INTERVAL);        // schedules a reconnection (still pending)
    wifi.tick(2 * W::STATUS_INTERVAL);    // another check with a reconnection pending
    TEST_ASSERT_EQUAL(1, WiFi.disconnectCalls);
}

void test_long_disconnection_schedules_restart() {
    W wifi(cfg, logger);
    WiFi._status = WL_DISCONNECTED;
    unsigned long t = 0;
    for (int i = 0; i < W::MAX_FAILURES; i++) {
        t += W::STATUS_INTERVAL;
        wifi.tick(t);
        wifi.tick(t + W::RECONNECT_DELAY);
    }
    TEST_ASSERT_TRUE(wifi.restartPending());
}

void test_connected_neither_probes_nor_restarts() {
    W wifi(cfg, logger);
    unsigned long t = 0;
    for (int i = 0; i < W::MAX_FAILURES; i++) {
        t += W::STATUS_INTERVAL;
        wifi.tick(t);
    }
    TEST_ASSERT_EQUAL(0, WiFi.disconnectCalls);
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(wifi.restartPending());
}

// ── tickRestart() ────────────────────────────────────────────────────────────

void test_critical_memory_needs_consecutive_checks() {
    W wifi(cfg, logger);
    ESP._heap = 1000;
    unsigned long t = 0;
    for (int i = 0; i < W::MEM_STRIKES - 1; i++) {
        t += W::MEM_CHECK_INTERVAL;
        wifi.tickRestart(t);
    }
    TEST_ASSERT_FALSE(wifi.restartPending());

    // Recovers: resets the counter
    ESP._heap = 50000;
    t += W::MEM_CHECK_INTERVAL;
    wifi.tickRestart(t);

    ESP._frag = 90;
    for (int i = 0; i < W::MEM_STRIKES - 1; i++) {
        t += W::MEM_CHECK_INTERVAL;
        wifi.tickRestart(t);
    }
    TEST_ASSERT_FALSE(wifi.restartPending());

    t += W::MEM_CHECK_INTERVAL;
    wifi.tickRestart(t);
    TEST_ASSERT_TRUE(wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "Critical memory"));
}

void test_memory_checked_only_on_interval() {
    W wifi(cfg, logger);
    ESP._heap = 1000;
    for (int i = 0; i < 10; i++) wifi.tickRestart(W::MEM_CHECK_INTERVAL);
    TEST_ASSERT_FALSE(wifi.restartPending());
}

void test_ota_holds_off_watchdogs() {
    W wifi(cfg, logger);
    wifi.setOTAInProgress(true);
    ESP._heap = 1000;
    for (int i = 1; i <= 10; i++) wifi.tickRestart(i * W::MEM_CHECK_INTERVAL);
    wifi.tickRestart(W::DAILY_RESTART + 1);
    TEST_ASSERT_FALSE(wifi.restartPending());
}

void test_daily_restart() {
    W wifi(cfg, logger);
    wifi.tickRestart(W::DAILY_RESTART + 1);
    TEST_ASSERT_TRUE(wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "Daily restart"));
}

void test_restart_runs_after_delay() {
    W wifi(cfg, logger);
    setMillis(1000);
    wifi.scheduleRestart(500);
    wifi.tickRestart(1499);
    TEST_ASSERT_EQUAL(0, ESP._restarts);
    wifi.tickRestart(1500);
    TEST_ASSERT_EQUAL(1, ESP._restarts);
}

void test_schedule_restart_ignores_second_request() {
    W wifi(cfg, logger);
    setMillis(0);
    wifi.scheduleRestart(100);
    wifi.scheduleRestart(100000);
    wifi.tickRestart(100);
    TEST_ASSERT_EQUAL(1, ESP._restarts);
}

void test_is_connected_reflects_status() {
    W wifi(cfg, logger);
    TEST_ASSERT_TRUE(wifi.isConnected());
    WiFi._status = WL_DISCONNECTED;
    TEST_ASSERT_FALSE(wifi.isConnected());
}

// ── Setup AP password ────────────────────────────────────────────────────────

static bool apKeyValid(const std::string& k) {
    if (k.size() != W::AP_KEY_LEN) return false;
    for (char c : k)
        if (!std::strchr("abcdefghijkmnpqrstuvwxyz23456789", c)) return false;
    return true;
}

void test_ap_password_drawn_and_saved() {
    cfg.ssid = "";
    W wifi(cfg, logger);
    wifi.begin();
    std::string pass = WiFi.apPass;
    TEST_ASSERT_TRUE(apKeyValid(pass));
    TEST_ASSERT_NOT_EQUAL(0, pass.compare("esp8266dns"));
    TEST_ASSERT_EQUAL_STRING(pass.c_str(), MockFS::files[W::AP_KEY_PATH].c_str());
    TEST_ASSERT_TRUE(logHas(logger, "AP: new password generated"));
    TEST_ASSERT_FALSE(logHas(logger, pass.c_str()));          // kept out of the panel log
    TEST_ASSERT_EQUAL_STRING(("AP " AP_SSID ", password: " + pass + "\n").c_str(), Serial.last.c_str());

    // Next boot: the same password, without drawing again
    logger = Logger();
    W again(cfg, logger);
    again.begin();
    TEST_ASSERT_EQUAL_STRING(pass.c_str(), WiFi.apPass.c_str());
    TEST_ASSERT_FALSE(logHas(logger, "new password"));
}

void test_ap_password_differs_between_devices() {
    W a(cfg, logger);
    std::string first = a.apPassword();
    MockFS::reset();
    ESP.random();                                             // another RNG state
    W b(cfg, logger);
    TEST_ASSERT_TRUE(apKeyValid(b.apPassword()));
    TEST_ASSERT_NOT_EQUAL(0, first.compare(b.apPassword()));
}

void test_broken_ap_password_is_drawn_again() {
    const char* bad[] = {"short", "abcdefghijk", "abcdefghijkmn", "ABCDEFGHJKMN", "abcdefghijk1", "abcdefghijk ", "abcdefghijk{"};
    for (const char* b : bad) {
        MockFS::reset();
        MockFS::put(W::AP_KEY_PATH, b);
        logger = Logger();
        W wifi(cfg, logger);
        std::string pass = wifi.apPassword();
        TEST_ASSERT_TRUE(apKeyValid(pass));
        TEST_ASSERT_TRUE(logHas(logger, "AP: new password generated"));
        TEST_ASSERT_EQUAL_STRING(pass.c_str(), MockFS::files[W::AP_KEY_PATH].c_str());
    }
    // Valid: stays as is
    MockFS::reset();
    MockFS::put(W::AP_KEY_PATH, "abcd2345wxyz");
    W ok(cfg, logger);
    TEST_ASSERT_EQUAL_STRING("abcd2345wxyz", ok.apPassword());
}

void test_unsaved_ap_password_lasts_the_boot() {
    MockFS::failWriteOpen = true;
    W wifi(cfg, logger);
    std::string pass = wifi.apPassword();
    TEST_ASSERT_TRUE(apKeyValid(pass));
    TEST_ASSERT_TRUE(logHas(logger, "AP: password not saved"));
    TEST_ASSERT_EQUAL_STRING(pass.c_str(), wifi.apPassword());   // the same for the whole boot
}

void test_ap_password_repeated_on_serial() {
    cfg.ssid = "";
    W wifi(cfg, logger);
    wifi.begin();
    int calls = Serial.printfCalls;
    wifi.tick(W::AP_ANNOUNCE_MS - 1);
    TEST_ASSERT_EQUAL(calls, Serial.printfCalls);
    wifi.tick(W::AP_ANNOUNCE_MS);
    TEST_ASSERT_EQUAL(calls + 1, Serial.printfCalls);
    TEST_ASSERT_TRUE(contains(Serial.last, "password: "));
    wifi.tick(W::AP_ANNOUNCE_MS + 1);
    TEST_ASSERT_EQUAL(calls + 1, Serial.printfCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_without_ssid_starts_protected_ap);
    RUN_TEST(test_connects_in_station_mode);
    RUN_TEST(test_hostname_before_connecting);
    RUN_TEST(test_begin_does_not_block);
    RUN_TEST(test_failure_starts_ap_and_station);
    RUN_TEST(test_got_ip_leaves_setup_mode);
    RUN_TEST(test_got_ip_in_station_mode_keeps_mode);
    RUN_TEST(test_got_ip_resets_failures);
    RUN_TEST(test_disconnected_event_logs);
    RUN_TEST(test_setup_mode_keeps_ap);
    RUN_TEST(test_reconnects_when_disconnected);
    RUN_TEST(test_does_not_start_duplicate_reconnection);
    RUN_TEST(test_long_disconnection_schedules_restart);
    RUN_TEST(test_connected_neither_probes_nor_restarts);
    RUN_TEST(test_critical_memory_needs_consecutive_checks);
    RUN_TEST(test_memory_checked_only_on_interval);
    RUN_TEST(test_ota_holds_off_watchdogs);
    RUN_TEST(test_daily_restart);
    RUN_TEST(test_restart_runs_after_delay);
    RUN_TEST(test_schedule_restart_ignores_second_request);
    RUN_TEST(test_is_connected_reflects_status);
    RUN_TEST(test_ap_password_drawn_and_saved);
    RUN_TEST(test_ap_password_differs_between_devices);
    RUN_TEST(test_broken_ap_password_is_drawn_again);
    RUN_TEST(test_unsaved_ap_password_lasts_the_boot);
    RUN_TEST(test_ap_password_repeated_on_serial);
    return UNITY_END();
}
