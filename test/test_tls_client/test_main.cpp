// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "TlsClient.h"

void setUp()    { resetMocks(); }
void tearDown() {}

static int get(TlsClient& c, String* out = nullptr, size_t cap = 2048, unsigned long ms = 1000) {
    return https::request(c, "h.example", "GET", "/p?q=1", nullptr, nullptr, ms, out, cap);
}

// ── TlsClient ────────────────────────────────────────────────────────────────

void test_prepare_sets_timeouts_buffers_and_suites() {
    TlsClient c;
    TEST_ASSERT_TRUE(c.prepare(1234));
    TEST_ASSERT_EQUAL(1234UL, MockTLS::lastTimeoutMs);
    TEST_ASSERT_EQUAL(2, MockTLS::timeoutCalls);                 // wrapper and context
    TEST_ASSERT_EQUAL(TlsClient::RX_BUF, MockTLS::bufIn);
    TEST_ASSERT_EQUAL(TlsClient::TX_BUF, MockTLS::bufOut);
    TEST_ASSERT_EQUAL(TlsClient::SUITE_COUNT, (int)MockTLS::suites.size());
    TEST_ASSERT_EQUAL_HEX16(0xC02B, MockTLS::suites[0]);

    MockTLS::nullOwned = true;                                   // no BearSSL context
    TlsClient d;
    TEST_ASSERT_FALSE(d.prepare(1000));
}

// ── https::request ───────────────────────────────────────────────────────────

void test_get_without_body() {
    TlsClient c;
    MockHTTP::push(200, "hello");
    String body;
    TEST_ASSERT_EQUAL(200, get(c, &body));
    TEST_ASSERT_EQUAL_STRING("hello", body.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::lastRaw,
        "GET /p?q=1 HTTP/1.0\r\nHost: h.example\r\nUser-Agent: ESP8266\r\nConnection: close\r\n\r\n"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("https://h.example/p?q=1", MockHTTP::requests[0].url.c_str());
    TEST_ASSERT_FALSE(c.isConnected());                          // closed after the answer
}

void test_post_with_headers_and_body() {
    TlsClient c;
    MockHTTP::push(201);
    int code = https::request(c, "h.example", "PATCH", "/r", "A: 1\r\nB: 2\r\n", "{\"x\":1}", 1000);
    TEST_ASSERT_EQUAL(201, code);
    const MockHTTP::Request& r = MockHTTP::requests[0];
    TEST_ASSERT_EQUAL_STRING("PATCH", r.method.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"x\":1}", r.payload.c_str());
    TEST_ASSERT_EQUAL_STRING("1", r.header("A").c_str());
    TEST_ASSERT_EQUAL_STRING("2", r.header("B").c_str());
    TEST_ASSERT_EQUAL_STRING("7", r.header("Content-Length").c_str());
}

void test_no_connection() {
    TlsClient c;
    MockHTTP::failNextBegin();
    TEST_ASSERT_EQUAL(https::NO_CONNECTION, get(c));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

// No status line, a status line that is not HTTP, or one without a code
void test_bad_responses() {
    TlsClient dropped;                                           // closed with nothing sent
    MockHTTP::push(-1);
    TEST_ASSERT_EQUAL(https::BAD_RESPONSE, get(dropped));
    const char* bad[] = {"HTTP/1.0 2", "garbage 200 OK\r\n\r\n", "HTTP/1.1\r\n\r\n",
                         "HTTP/1.1 200 OK\r\nX: y"};
    for (const char* raw : bad) {
        TlsClient c;
        MockHTTP::rawHttp = raw;
        TEST_ASSERT_EQUAL(https::BAD_RESPONSE, get(c));
        TEST_ASSERT_FALSE(c.isConnected());
    }
}

// The body ends at Content-Length (any case), even with the connection open
void test_body_stops_at_content_length() {
    TlsClient c;
    MockHTTP::keepOpen = true;
    MockHTTP::rawHttp = "HTTP/1.1 200 OK\r\ncontent-LENGTH: 3\r\n\r\nabcdef";
    String body;
    setMillis(0);
    TEST_ASSERT_EQUAL(200, get(c, &body));
    TEST_ASSERT_EQUAL_STRING("abc", body.c_str());
    TEST_ASSERT_EQUAL(0UL, millis());                            // did not wait for anything
}

// Without Content-Length the body goes until the server closes; cap limits it
void test_body_until_close_and_cap() {
    TlsClient c;
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\nServer: x\r\n\r\n0123456789";
    String body;
    TEST_ASSERT_EQUAL(200, get(c, &body));
    TEST_ASSERT_EQUAL_STRING("0123456789", body.c_str());

    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\n\r\n0123456789";
    String small;
    TEST_ASSERT_EQUAL(200, get(c, &small, 4));
    TEST_ASSERT_EQUAL_STRING("0123", small.c_str());

    MockHTTP::rawHttp = "HTTP/1.0 204 No Content\r\n\r\nignored";
    TEST_ASSERT_EQUAL(204, get(c));                              // nobody asked for the body
}

// A server that stops sending: each wait ends after ms
void test_stalled_server_times_out() {
    TlsClient c;
    MockHTTP::keepOpen = true;
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\n";
    setMillis(0);
    TEST_ASSERT_EQUAL(https::BAD_RESPONSE, get(c, nullptr, 2048, 500));
    TEST_ASSERT_EQUAL(500UL, millis());

    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\n\r\npartial";
    String body;
    setMillis(0);
    TEST_ASSERT_EQUAL(200, get(c, &body, 2048, 500));
    TEST_ASSERT_EQUAL_STRING("partial", body.c_str());
    TEST_ASSERT_EQUAL(500UL, millis());
}

// A header line longer than the buffer is cut, and the next one still reads
void test_long_header_line() {
    TlsClient c;
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\nX-Long: " + std::string(200, 'z') +
                        "\r\nContent-Length: 2\r\n\r\nok!";
    String body;
    TEST_ASSERT_EQUAL(200, get(c, &body));
    TEST_ASSERT_EQUAL_STRING("ok", body.c_str());
}

// A body in parts, some in flash, and the answer handed over byte by byte
static void collect(void* ctx, char c) { static_cast<std::string*>(ctx)->push_back(c); }

void test_exchange_parts_and_sink() {
    static const char FLASH[] PROGMEM = "-flash-";
    const https::Part parts[] = {{"ram", 3, false}, {FLASH, sizeof(FLASH) - 1, true}, {"end", 3, false}};
    MockHTTP::push(200, "the whole answer");
    TlsClient c;
    std::string got;
    int code = https::exchange(c, "h.example", "PUT", "/x", "X-A: 1\r\n", parts, 3, 1000, collect, &got);
    TEST_ASSERT_EQUAL(200, code);
    TEST_ASSERT_EQUAL_STRING("the whole answer", got.c_str());
    TEST_ASSERT_EQUAL_STRING("ram-flash-end", MockHTTP::requests[0].payload.c_str());
    TEST_ASSERT_EQUAL_STRING("13", MockHTTP::requests[0].header("Content-Length").c_str());
    TEST_ASSERT_EQUAL_STRING("1", MockHTTP::requests[0].header("X-A").c_str());

    // No parts: no Content-Length; no sink: the body is not read
    MockHTTP::push(204, "ignored");
    TlsClient d;
    TEST_ASSERT_EQUAL(204, https::exchange(d, "h.example", "GET", "/", nullptr, nullptr, 0, 1000));
    TEST_ASSERT_EQUAL_STRING("", MockHTTP::requests[1].header("Content-Length").c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_prepare_sets_timeouts_buffers_and_suites);
    RUN_TEST(test_get_without_body);
    RUN_TEST(test_post_with_headers_and_body);
    RUN_TEST(test_no_connection);
    RUN_TEST(test_bad_responses);
    RUN_TEST(test_body_stops_at_content_length);
    RUN_TEST(test_body_until_close_and_cap);
    RUN_TEST(test_stalled_server_times_out);
    RUN_TEST(test_long_header_line);
    RUN_TEST(test_exchange_parts_and_sink);
    return UNITY_END();
}
