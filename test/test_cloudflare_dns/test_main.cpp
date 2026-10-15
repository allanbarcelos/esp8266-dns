// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "Clock.h"
#include "WiFiService.h"
#include "CloudflareDNS.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

static const char* OK_JSON  = R"({"success":true,"errors":[],"result":{"id":"x","content":"1.2.3.4"}})";
static const char* ERR_JSON = R"({"success":false,"errors":[{"code":10000,"message":"Authentication error"}]})";

// Public IP already known (PublicIPResolver)
void setUp() {
    resetMocks();
    logger = Logger();
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    cfg = Config{};
    strlcpy(cfg.cf_token,  "TOKEN",   sizeof(cfg.cf_token));
    strlcpy(cfg.cf_zone,   "ZONE",    sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_record, "REC0",    sizeof(cfg.cf_record));
    strlcpy(cfg.publicIP,  "1.2.3.4", sizeof(cfg.publicIP));
}
void tearDown() {}

struct Fixture {
    Clock         clock{fakeTime};
    WiFiService   wifi{cfg, logger};
    CloudflareDNS dns{cfg, logger, wifi, clock};
};

static const unsigned long T0 = 1000;
static const unsigned long R  = CloudflareDNS::RETRY_INTERVAL;

void test_configured_needs_token_zone_and_record() {
    Fixture f;
    TEST_ASSERT_TRUE(f.dns.configured());
    cfg.cf_record[0] = '\0';
    TEST_ASSERT_FALSE(f.dns.configured());
    strlcpy(cfg.cf_record, "REC0", sizeof(cfg.cf_record));
    cfg.cf_zone[0] = '\0';
    TEST_ASSERT_FALSE(f.dns.configured());
    strlcpy(cfg.cf_zone, "ZONE", sizeof(cfg.cf_zone));
    cfg.cf_token[0] = '\0';
    TEST_ASSERT_FALSE(f.dns.configured());

    f.dns.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_without_public_ip_waits() {
    cfg.publicIP[0] = '\0';
    Fixture f;
    TEST_ASSERT_FALSE(f.dns.due(T0));
    f.dns.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_without_wifi_or_clock_does_not_update() {
    Fixture f;
    WiFi._status = WL_DISCONNECTED;
    f.dns.tick(T0);
    WiFi._status = WL_CONNECTED;
    fakeNow = 1000;
    f.dns.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_updates_the_record() {
    Fixture f;
    setMillis(T0);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T0);

    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    const auto& p = MockHTTP::requests[0];
    TEST_ASSERT_EQUAL_STRING("PATCH", p.method.c_str());
    TEST_ASSERT_EQUAL_STRING("https://api.cloudflare.com/client/v4/zones/ZONE/dns_records/REC0", p.url.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"content\":\"1.2.3.4\"}", p.payload.c_str());
    TEST_ASSERT_EQUAL_STRING("Bearer TOKEN", p.header("Authorization").c_str());
    TEST_ASSERT_EQUAL_STRING("application/json", p.header("Content-Type").c_str());

    // TLS validated with the embedded roots and the current clock
    TEST_ASSERT_TRUE(cloudflareTrust(MockTLS::lastTrust));
    TEST_ASSERT_EQUAL((long)fakeNow, (long)MockTLS::lastTime);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);

    TEST_ASSERT_EQUAL_STRING("1.2.3.4", f.dns.syncedIP());
    TEST_ASSERT_EQUAL(T0, cfg.lastDnsUpdate);
    TEST_ASSERT_TRUE(logHas(logger, "Updating DNS to 1.2.3.4"));
    TEST_ASSERT_TRUE(logHas(logger, "DNS updated!"));
}

void test_same_ip_sends_no_patch() {
    Fixture f;
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T0);
    TEST_ASSERT_FALSE(f.dns.due(T0 + 10 * R));
    f.dns.tick(T0 + 10 * R);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
}

void test_changed_ip_sends_patch_again() {
    Fixture f;
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T0);

    strlcpy(cfg.publicIP, "5.6.7.8", sizeof(cfg.publicIP));
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T0 + R);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("{\"content\":\"5.6.7.8\"}", MockHTTP::requests[1].payload.c_str());
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", f.dns.syncedIP());
}

// A failure retries every minute, even with the same IP
void test_api_error_retries_in_a_minute() {
    Fixture f;
    MockHTTP::push(403, ERR_JSON);
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: Authentication error"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
    TEST_ASSERT_EQUAL(0, cfg.lastDnsUpdate);

    TEST_ASSERT_FALSE(f.dns.due(T0 + R - 1));
    f.dns.tick(T0 + R - 1);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());

    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T0 + R);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", f.dns.syncedIP());
}

void test_error_without_details() {
    Fixture f;
    MockHTTP::push(500, R"({"success":false})");
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: HTTP 500"));
}

void test_invalid_json_response() {
    Fixture f;
    MockHTTP::push(502, "<html>Bad gateway</html>");
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: HTTP 502"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

void test_truncated_json_response() {
    Fixture f;
    MockHTTP::push(200, "{\"success\":");
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: HTTP 200"));
}

// A real PATCH response: the full record, nested objects and lists, commas
// inside text. Read whole, with no filter.
static const char* FULL_JSON =
    R"({"result":{"id":"023e105f4ecef8ad9ca31a8372d0c353","zone_id":"023e105f4ecef8ad9ca31a8372d0c353",)"
    R"("zone_name":"example.com","name":"home.example.com","type":"A","content":"1.2.3.4",)"
    R"("proxiable":true,"proxied":false,"ttl":1,"settings":{"ipv4_only":true,"ipv6_only":false},)"
    R"("meta":{"auto_added":false,"managed_by_apps":false,"managed_by_argo_tunnel":false},)"
    R"("comment":"home, office, [test] {x}","tags":["a:1","b:2"],)"
    R"("created_on":"2014-01-01T05:20:00.12345Z","modified_on":"2014-01-01T05:20:00.12345Z"},)"
    R"("success":true,"errors":[],"messages":[{"code":1000,"message":"ok, done"}]})";

void test_full_api_response() {
    Fixture f;
    MockHTTP::push(200, FULL_JSON);
    f.dns.tick(T0);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", f.dns.syncedIP());
    TEST_ASSERT_TRUE(logHas(logger, "DNS updated!"));
}

void test_error_message_with_commas() {
    Fixture f;
    MockHTTP::push(400, R"({"success":false,"errors":[{"code":9005,"message":"Content, for A record, is invalid"}],"messages":[],"result":null})");
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: Content, for A record, is invalid"));
}

// Valid JSON, but without "success" (proxy, another API): invalid
void test_response_without_success_field() {
    Fixture f;
    MockHTTP::push(200, R"({"result":{"id":"x"},"errors":[]})");
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: HTTP 200"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

// "success" that is not true is a failure; null counts as absent; errors
// that are not a list, or fields of other types, leave the defaults
void test_odd_answer_types() {
    Fixture f;
    MockHTTP::push(200, R"({"success":"yes","errors":{"code":5}})");
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: HTTP 200"));
    logger = Logger();
    MockHTTP::push(200, R"({"success":null})");
    f.dns.tick(T0 + R);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: HTTP 200"));
    logger = Logger();
    MockHTTP::push(200, R"({"success":false,"errors":[{"code":"x","message":7}]})");
    f.dns.tick(T0 + 2 * R);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: 7"));
    MockHTTP::push(200, R"({"success":false,"errors":[]})");
    f.dns.tick(T0 + 3 * R);
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

// The TLS session closes before the response is parsed
void test_tls_session_closes_before_parsing() {
    Fixture f;
    MockHTTP::push(200, OK_JSON);
    MockTLS::stopCalls = 0;
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(MockTLS::stopCalls >= 1);
}

void test_tls_connection_error() {
    Fixture f;
    MockHTTP::push(-1);
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: no connection (-2)"));
}

void test_connection_start_fails() {
    Fixture f;
    MockHTTP::failNextBegin();
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: no connection (-1)"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

void test_dns_not_enough_memory() {
    Fixture f;
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: not enough memory"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_dns_tls_unavailable() {
    Fixture f;
    MockTLS::nullOwned = true;
    f.dns.tick(T0);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: TLS unavailable"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

void test_success_resets_wifi_failures() {
    Fixture f;
    for (int i = 0; i < WiFiService::MAX_FAILURES - 1; i++) f.wifi.notifyFailure();
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T0);
    f.wifi.notifyFailure();
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

// ntfy releases TLS when the next DNS tick will use the network
void test_due_tells_when_it_will_use_the_network() {
    Fixture f;
    TEST_ASSERT_TRUE(f.dns.due(0));
    WiFi._status = WL_DISCONNECTED;
    TEST_ASSERT_FALSE(f.dns.due(T0));
    WiFi._status = WL_CONNECTED;
    fakeNow = 1000;
    TEST_ASSERT_FALSE(f.dns.due(T0));
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    cfg.cf_record[0] = '\0';
    TEST_ASSERT_FALSE(f.dns.due(T0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_configured_needs_token_zone_and_record);
    RUN_TEST(test_without_public_ip_waits);
    RUN_TEST(test_without_wifi_or_clock_does_not_update);
    RUN_TEST(test_updates_the_record);
    RUN_TEST(test_same_ip_sends_no_patch);
    RUN_TEST(test_changed_ip_sends_patch_again);
    RUN_TEST(test_api_error_retries_in_a_minute);
    RUN_TEST(test_error_without_details);
    RUN_TEST(test_invalid_json_response);
    RUN_TEST(test_truncated_json_response);
    RUN_TEST(test_full_api_response);
    RUN_TEST(test_error_message_with_commas);
    RUN_TEST(test_response_without_success_field);
    RUN_TEST(test_odd_answer_types);
    RUN_TEST(test_tls_session_closes_before_parsing);
    RUN_TEST(test_tls_connection_error);
    RUN_TEST(test_connection_start_fails);
    RUN_TEST(test_dns_not_enough_memory);
    RUN_TEST(test_dns_tls_unavailable);
    RUN_TEST(test_success_resets_wifi_failures);
    RUN_TEST(test_due_tells_when_it_will_use_the_network);
    return UNITY_END();
}
