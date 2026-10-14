// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Clock.h"
#include "PublicIPResolver.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
}
void tearDown() {}

struct Fix {
    Clock            clock{fakeTime};
    PublicIPResolver resolver{cfg, logger, clock};
};

static const unsigned long T0 = 1000;
static const unsigned long CHECK = PublicIPResolver::CHECK_INTERVAL;
static const unsigned long RETRY = PublicIPResolver::RETRY_INTERVAL;

static int logCount(const char* needle) {
    int n = 0;
    for (uint8_t i = 0; i < logger.count(); i++)
        if (std::strstr(logger.entry(logger.startIndex() + i), needle)) n++;
    return n;
}

void test_first_service_ok() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);
    TEST_ASSERT_EQUAL(T0, cfg.ipChanged);
    TEST_ASSERT_EQUAL(T0, cfg.ipChecked);
    TEST_ASSERT_TRUE(logHas(logger, "Public IP: 1.2.3.4"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("https://api4.ipify.org", MockHTTP::calledURLs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("ESP8266", MockHTTP::requests[0].headers[0].second.c_str());
    TEST_ASSERT_TRUE(cloudflareTrust(MockTLS::lastTrust));
    TEST_ASSERT_EQUAL((long)fakeNow, (long)MockTLS::lastTime);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
    // the wrapper and the BearSSL context get the same budget
    TEST_ASSERT_EQUAL(2, MockTLS::timeoutCalls);
    TEST_ASSERT_EQUAL(PublicIPResolver::CONNECT_TIMEOUT, MockTLS::lastTimeoutMs);
    TEST_ASSERT_EQUAL(TlsClient::RX_BUF, MockTLS::bufIn);
    TEST_ASSERT_EQUAL(TlsClient::TX_BUF, MockTLS::bufOut);
    // Only ECDHE + AES-GCM: the other ciphers are not even in the firmware (TlsTrim.h)
    const std::vector<uint16_t> gcm = {0xC02B, 0xC02F, 0xC02C, 0xC030};
    TEST_ASSERT_TRUE(MockTLS::suites == gcm);
    // New IP: asks the network's DNS for the reverse name
    TEST_ASSERT_EQUAL(1, MockUDP::live);
    TEST_ASSERT_EQUAL(53, MockUDP::port);
}

// With neither DNS nor ntfy set up: the device checks on its own
void test_checks_every_5_min_and_logs_only_changes() {
    Fix f;
    TEST_ASSERT_TRUE(f.resolver.due(0));
    MockHTTP::push(200, "1.2.3.4");
    f.resolver.tick(T0);
    TEST_ASSERT_FALSE(f.resolver.due(T0 + CHECK - 1));
    f.resolver.tick(T0 + CHECK - 1);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());

    // Same IP: nothing in the log, only the check time
    uint8_t lines = logger.count();
    MockHTTP::push(200, "1.2.3.4");
    TEST_ASSERT_TRUE(f.resolver.due(T0 + CHECK));
    f.resolver.tick(T0 + CHECK);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(lines, logger.count());
    TEST_ASSERT_EQUAL(T0, cfg.ipChanged);
    TEST_ASSERT_EQUAL(T0 + CHECK, cfg.ipChecked);

    // Changed: logs the old and the new one, reverse again
    strlcpy(cfg.publicHost, "old.example", sizeof(cfg.publicHost));
    MockHTTP::push(200, "5.6.7.8");
    f.resolver.tick(T0 + 2 * CHECK);
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", cfg.publicIP);
    TEST_ASSERT_TRUE(logHas(logger, "Public IP changed: 1.2.3.4 -> 5.6.7.8"));
    TEST_ASSERT_EQUAL(T0 + 2 * CHECK, cfg.ipChanged);
    TEST_ASSERT_EQUAL_STRING("", cfg.publicHost);
    TEST_ASSERT_EQUAL(1, MockUDP::live);
}

void test_without_wifi_or_clock_does_not_check() {
    Fix f;
    WiFi._status = WL_DISCONNECTED;
    TEST_ASSERT_FALSE(f.resolver.due(T0));
    f.resolver.tick(T0);
    WiFi._status = WL_CONNECTED;
    fakeNow = 1000;
    TEST_ASSERT_FALSE(f.resolver.due(T0));
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(0, logger.count());
}

// A failure is logged only the first time; retries every minute
void test_failure_logs_once_and_retries_every_minute() {
    Fix f;
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(500);
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(NUM_IP_SERVICES, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("", cfg.publicIP);
    TEST_ASSERT_EQUAL(1, logCount("no service answered"));
    TEST_ASSERT_FALSE(f.resolver.due(T0 + RETRY - 1));
    TEST_ASSERT_TRUE(f.resolver.due(T0 + RETRY));

    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(-1);
    f.resolver.tick(T0 + RETRY);
    TEST_ASSERT_EQUAL(1, logCount("no service answered"));

    // Back: normal interval, and a new failure is logged again
    MockHTTP::push(200, "1.2.3.4");
    f.resolver.tick(T0 + 2 * RETRY);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);
    TEST_ASSERT_FALSE(f.resolver.due(T0 + 2 * RETRY + RETRY));
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(500);
    f.resolver.tick(T0 + 2 * RETRY + CHECK);
    TEST_ASSERT_EQUAL(2, logCount("no service answered"));
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);   // keeps the last known one
}

void test_not_enough_memory_does_not_check() {
    Fix f;
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "Public IP: not enough memory"));

    ESP._heap = 50000;
    ESP._maxBlock = PublicIPResolver::MIN_TLS_BLOCK - 1;
    f.resolver.tick(T0 + RETRY);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(1, logCount("not enough memory"));
}

void test_tls_unavailable() {
    Fix f;
    MockTLS::nullOwned = true;
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "no service answered"));
}

void test_accepts_spaces_and_newline() {
    Fix f;
    MockHTTP::push(200, "  5.6.7.8\n");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", cfg.publicIP);
}

void test_connection_failure_tries_next() {
    Fix f;
    MockHTTP::failNextBegin();
    MockHTTP::push(200, "9.9.9.9");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL_STRING("9.9.9.9", cfg.publicIP);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("https://icanhazip.com", MockHTTP::calledURLs[1].c_str());
}

void test_errors_try_the_next() {
    Fix f;
    MockHTTP::push(503);
    MockHTTP::push(200, "<html>error</html>");
    MockHTTP::push(200, "10.0.0.1");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", cfg.publicIP);
    TEST_ASSERT_EQUAL_STRING("https://ifconfig.co/ip", MockHTTP::calledURLs[2].c_str());
    TEST_ASSERT_FALSE(logHas(logger, "no service"));
}

void test_reverse_without_answer_gives_up() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(1, MockUDP::live);
    f.resolver.tick(T0 + ReverseDNS::TIMEOUT - 1);
    TEST_ASSERT_EQUAL(1, MockUDP::live);
    f.resolver.tick(T0 + ReverseDNS::TIMEOUT);
    TEST_ASSERT_EQUAL(0, MockUDP::live);
    TEST_ASSERT_EQUAL_STRING("", cfg.publicHost);
}

// The generated CloudflareCA.h: each root is a complete DER certificate
// (SEQUENCE with a long length), and the lengths cover the whole block
void test_whole_der_roots() {
    size_t off = 0;
    for (uint16_t len : CLOUDFLARE_LEN) {
        const uint8_t* d = CLOUDFLARE_DER + off;
        TEST_ASSERT_EQUAL_HEX8(0x30, d[0]);
        TEST_ASSERT_EQUAL_HEX8(0x82, d[1]);
        TEST_ASSERT_EQUAL(len, ((d[2] << 8) | d[3]) + 4);
        off += len;
    }
    TEST_ASSERT_EQUAL(sizeof(CLOUDFLARE_DER), off);
    TEST_ASSERT_EQUAL(4, (int)(sizeof(CLOUDFLARE_LEN) / sizeof(CLOUDFLARE_LEN[0])));

    BearSSL::X509List list;
    cloudflareRoots(list);
    TEST_ASSERT_TRUE(cloudflareTrust(list.trust));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_service_ok);
    RUN_TEST(test_checks_every_5_min_and_logs_only_changes);
    RUN_TEST(test_without_wifi_or_clock_does_not_check);
    RUN_TEST(test_failure_logs_once_and_retries_every_minute);
    RUN_TEST(test_not_enough_memory_does_not_check);
    RUN_TEST(test_tls_unavailable);
    RUN_TEST(test_accepts_spaces_and_newline);
    RUN_TEST(test_connection_failure_tries_next);
    RUN_TEST(test_errors_try_the_next);
    RUN_TEST(test_reverse_without_answer_gives_up);
    RUN_TEST(test_whole_der_roots);
    return UNITY_END();
}
