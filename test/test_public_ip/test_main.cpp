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
    TEST_ASSERT_EQUAL_STRING("https://api4.ipify.org/", MockHTTP::calledURLs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("ESP8266", MockHTTP::requests[0].header("User-Agent").c_str());
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
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());     // the first did not connect
    TEST_ASSERT_EQUAL_STRING("https://icanhazip.com/", MockHTTP::calledURLs[0].c_str());
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

// The generated CloudflareCA.h: four anchors (two RSA-4096 and two P-384),
// each with its name and key, read whole into the X509List
void test_trust_anchors() {
    BearSSL::X509List list;
    cloudflareRoots(list);
    TEST_ASSERT_TRUE(cloudflareTrust(list.trust()));
    const br_x509_trust_anchor* ta = list.getTrustAnchors();
    const unsigned char kinds[] = {BR_KEYTYPE_RSA, BR_KEYTYPE_EC, BR_KEYTYPE_RSA, BR_KEYTYPE_EC};
    for (size_t i = 0; i < CLOUDFLARE_ROOTS; i++) {
        TEST_ASSERT_EQUAL(kinds[i], ta[i].pkey.key_type);
        TEST_ASSERT_EQUAL_HEX8(0x30, ta[i].dn.data[0]);          // the name is a DER SEQUENCE
        TEST_ASSERT_EQUAL(ta[i].dn.len, (size_t)ta[i].dn.data[1] + 2);
    }
    TEST_ASSERT_EQUAL(512, (int)ta[0].pkey.key.rsa.nlen);
    TEST_ASSERT_EQUAL(3, (int)ta[0].pkey.key.rsa.elen);           // 65537
    TEST_ASSERT_EQUAL(24, ta[1].pkey.key.ec.curve);               // P-384
    TEST_ASSERT_EQUAL(97, (int)ta[1].pkey.key.ec.qlen);
    TEST_ASSERT_EQUAL_HEX8(0x04, ta[1].pkey.key.ec.q[0]);         // uncompressed point
}

// Out of memory, the list keeps only the complete anchors (a missing root
// fails the validation, closed) and frees everything (ASan checks)
void test_trust_anchors_out_of_memory() {
    struct Case { size_t size; size_t left; };
    const Case cases[] = {
        {CLOUDFLARE_ROOTS * sizeof(br_x509_certificate), 0},
        {CLOUDFLARE_ROOTS * sizeof(br_x509_trust_anchor), 0},
        {73, 0},    // GTS Root R1's name
        {512, 0},   // GTS Root R1's modulus
        {3, 0},     // GTS Root R1's exponent
        {97, 1},    // GTS Root R4's point
        {81, 2},    // ISRG Root X1's name
    };
    for (const Case& c : cases) {
        BearSSL::X509List list;
        mock::failMallocSize = c.size;
        cloudflareRoots(list);
        TEST_ASSERT_EQUAL(0, (int)mock::failMallocSize);   // that allocation did fail
        TEST_ASSERT_EQUAL(c.left, list.getCount());
        if (c.left) TEST_ASSERT_TRUE(list.trust().complete);
    }
}

// The relay's Worker saw the address: it counts as the check, no TLS
void test_address_offered_by_the_relay() {
    Fix f;
    f.resolver.offer("203.0.113.7", T0);
    TEST_ASSERT_EQUAL_STRING("203.0.113.7", cfg.publicIP);
    TEST_ASSERT_EQUAL(T0, cfg.ipChanged);
    TEST_ASSERT_EQUAL(T0, cfg.ipChecked);
    TEST_ASSERT_TRUE(logHas(logger, "Public IP: 203.0.113.7"));
    TEST_ASSERT_FALSE(f.resolver.due(T0 + CHECK - 1));
    TEST_ASSERT_TRUE(f.resolver.due(T0 + CHECK));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    // The same again: only the time of the check; not an address: nothing
    f.resolver.offer("203.0.113.7", T0 + 10);
    TEST_ASSERT_EQUAL(1, logCount("Public IP"));
    TEST_ASSERT_EQUAL(T0 + 10, cfg.ipChecked);
    f.resolver.offer("2001:db8::1", T0 + 20);
    f.resolver.offer("999999999999999", T0 + 20);
    TEST_ASSERT_EQUAL(T0 + 10, cfg.ipChecked);
    TEST_ASSERT_EQUAL_STRING("203.0.113.7", cfg.publicIP);
    // A new one is logged as a change
    f.resolver.offer("203.0.113.8", T0 + 30);
    TEST_ASSERT_TRUE(logHas(logger, "Public IP changed: 203.0.113.7 -> 203.0.113.8"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_service_ok);
    RUN_TEST(test_address_offered_by_the_relay);
    RUN_TEST(test_checks_every_5_min_and_logs_only_changes);
    RUN_TEST(test_without_wifi_or_clock_does_not_check);
    RUN_TEST(test_failure_logs_once_and_retries_every_minute);
    RUN_TEST(test_not_enough_memory_does_not_check);
    RUN_TEST(test_tls_unavailable);
    RUN_TEST(test_accepts_spaces_and_newline);
    RUN_TEST(test_connection_failure_tries_next);
    RUN_TEST(test_errors_try_the_next);
    RUN_TEST(test_reverse_without_answer_gives_up);
    RUN_TEST(test_trust_anchors);
    RUN_TEST(test_trust_anchors_out_of_memory);
    return UNITY_END();
}
