// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "HttpServer.h"
#include "../web_browser.h"

// Test handler: keeps the request and answers by path
struct Probe {
    HttpServer  srv{80};
    MockBrowser b{[this]() { srv.handleClient(); }};
    int         calls = 0;
    uint8_t     method = 0;
    std::string path, auth, plain;

    std::function<void(Probe&)> reply = [](Probe& p) { p.srv.send(200, "text/plain", "ok"); };

    Probe() { srv.begin(&Probe::handle, this); }

    static void handle(void* self) {
        Probe& p = *static_cast<Probe*>(self);
        p.calls++;
        p.method = p.srv.method();
        p.path = p.srv.path();
        p.auth = p.srv.authorization().c_str();
        p.plain = p.srv.arg("plain").c_str();
        p.reply(p);
    }

    std::string arg(const char* n) { return srv.arg(n).c_str(); }

    // A connection waiting in the queue, with what the browser already sent
    std::shared_ptr<MockConn> connect(const std::string& rx = "") {
        auto c = std::make_shared<MockConn>();
        c->rx = rx;
        MockServer::pending.push_back(c);
        return c;
    }
};

void setUp()    { resetMocks(); }
void tearDown() {}

// ── request ──────────────────────────────────────────────────────────────────

void test_get_with_query_and_sized_response() {
    Probe p;
    TEST_ASSERT_TRUE(MockServer::begun);
    p.b.send("GET /x/y?a=1&b=dois+tres&c=%41%62%2f HTTP/1.1\r\nHost: e\r\n\r\n");
    TEST_ASSERT_EQUAL(1, p.calls);
    TEST_ASSERT_EQUAL(HttpServer::GET, p.method);
    TEST_ASSERT_EQUAL_STRING("/x/y", p.path.c_str());
    TEST_ASSERT_EQUAL_STRING("1", p.arg("a").c_str());
    TEST_ASSERT_EQUAL_STRING("dois tres", p.arg("b").c_str());
    TEST_ASSERT_EQUAL_STRING("Ab/", p.arg("c").c_str());
    TEST_ASSERT_EQUAL_STRING("", p.arg("d").c_str());
    TEST_ASSERT_EQUAL_STRING("", p.plain.c_str());

    TEST_ASSERT_EQUAL(200, p.b.code);
    TEST_ASSERT_EQUAL_STRING("text/plain", p.b.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("2", p.b.headers["Content-Length"].c_str());
    TEST_ASSERT_EQUAL_STRING("close", p.b.headers["Connection"].c_str());
    TEST_ASSERT_EQUAL_STRING("ok", p.b.out.c_str());
    TEST_ASSERT_TRUE(p.b.conn->closed);
}

// Like the core: query first, urlencoded body next; the raw body in plain
void test_post_urlencoded_and_plain() {
    Probe p;
    p.b.send("POST /s?a=q HTTP/1.1\r\ncontent-type: application/x-www-form-urlencoded; charset=UTF-8\r\n"
             "CONTENT-LENGTH: 16\r\n\r\na=b&on&name=x%20");
    TEST_ASSERT_EQUAL(HttpServer::POST, p.method);
    TEST_ASSERT_EQUAL_STRING("q", p.arg("a").c_str());
    TEST_ASSERT_EQUAL_STRING("", p.arg("on").c_str());
    TEST_ASSERT_EQUAL_STRING("x ", p.arg("name").c_str());
    TEST_ASSERT_EQUAL_STRING("a=b&on&name=x%20", p.plain.c_str());
}

// Another body type (JSON, text): only in plain
void test_body_that_is_not_a_form() {
    Probe p;
    p.b.send("POST /s HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 7\r\n\r\ncmd=abc");
    TEST_ASSERT_EQUAL_STRING("", p.arg("cmd").c_str());
    TEST_ASSERT_EQUAL_STRING("cmd=abc", p.plain.c_str());
}

void test_decoding_and_similar_names() {
    Probe p;
    p.b.send("GET /?ab=1&a&a=2&x=%zz%4z%4&y=%&z=50%25 HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("", p.arg("a").c_str());          // "a" with no value comes first
    TEST_ASSERT_EQUAL_STRING("1", p.arg("ab").c_str());
    TEST_ASSERT_EQUAL_STRING("%zz%4z%4", p.arg("x").c_str());     // % without two hex digits stays
    TEST_ASSERT_EQUAL_STRING("%", p.arg("y").c_str());
    TEST_ASSERT_EQUAL_STRING("50%", p.arg("z").c_str());
    TEST_ASSERT_EQUAL_STRING("", p.arg("abc").c_str());
}

// A header without a colon is ignored; a long Authorization is cut
void test_headers() {
    Probe p;
    std::string longAuth(HttpServer::MAX_AUTH + 100, 'k');
    p.b.send("GET / HTTP/1.1\r\nlixo\r\nAuthorization:   Basic abc\r\nX-Outro: 1\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("Basic abc", p.auth.c_str());
    p.b.send("GET / HTTP/1.1\nAuthorization: " + longAuth + "\n\n");   // \n alone also works
    TEST_ASSERT_EQUAL(2, p.calls);
    TEST_ASSERT_TRUE(p.auth.size() > HttpServer::MAX_AUTH);
    TEST_ASSERT_TRUE(p.auth.size() < longAuth.size());
    p.b.send("GET / HTTP/1.1\r\n\r\n");                                 // does not leak between requests
    TEST_ASSERT_EQUAL_STRING("", p.auth.c_str());
}

void test_unknown_method_reaches_handler() {
    Probe p;
    p.b.send("DELETE /x HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL(HttpServer::OTHER, p.method);
}

// ── requests refused before the handler ──────────────────────────────────────

void test_malformed_request() {
    Probe p;
    const char* bad[] = {"GET\r\n\r\n", "GET /\r\n\r\n", "lixo"};
    for (const char* r : bad) {
        p.b.send(r);
        TEST_ASSERT_EQUAL(400, p.b.code);
    }
    // Headers that never end
    p.b.send("GET / HTTP/1.1\r\nHost: e\r\n");
    TEST_ASSERT_EQUAL(400, p.b.code);
    TEST_ASSERT_EQUAL(0, p.calls);
}

void test_path_too_long() {
    Probe p;
    p.b.send("GET /" + std::string(sizeof("/update/install") + 10, 'a') + " HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL(404, p.b.code);
    TEST_ASSERT_EQUAL(0, p.calls);
}

void test_body_too_large_or_negative() {
    Probe p;
    p.b.send("POST / HTTP/1.1\r\nContent-Length: " + std::to_string(HttpServer::MAX_BODY + 1) + "\r\n\r\n");
    TEST_ASSERT_EQUAL(413, p.b.code);
    p.b.send("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n");
    TEST_ASSERT_EQUAL(413, p.b.code);
    TEST_ASSERT_EQUAL(0, p.calls);

    p.b.send("POST / HTTP/1.1\r\nContent-Length: " + std::to_string(HttpServer::MAX_BODY) + "\r\n\r\n" +
             std::string(HttpServer::MAX_BODY, 'x'));
    TEST_ASSERT_EQUAL(1, p.calls);
    TEST_ASSERT_EQUAL((int)HttpServer::MAX_BODY, (int)p.plain.size());
}

// The body stops halfway: waits READ_MS and gives up
void test_incomplete_body() {
    Probe p;
    setMillis(1000);
    p.b.send("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    TEST_ASSERT_EQUAL(400, p.b.code);
    TEST_ASSERT_TRUE(millis() >= 1000 + HttpServer::READ_MS);
    TEST_ASSERT_EQUAL(0, p.calls);

    // The browser closes halfway: gives up right away
    auto c = p.connect("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    c->open = false;
    setMillis(5000);
    p.srv.handleClient();
    TEST_ASSERT_EQUAL(5000UL, millis());
    TEST_ASSERT_TRUE(c->closed);
    TEST_ASSERT_EQUAL(0, p.calls);
}

// ── connection without a request ─────────────────────────────────────────────

void test_idle_connection_waits_and_gives_up() {
    Probe p;
    p.srv.handleClient();                    // nobody at the port
    setMillis(100);
    auto c = p.connect();
    p.srv.handleClient();
    setMillis(100 + HttpServer::WAIT_MS - 1);
    p.srv.handleClient();
    TEST_ASSERT_FALSE(c->closed);

    // The request arrives later, on the same connection
    c->rx = "GET /late HTTP/1.1\r\n\r\n";
    p.srv.handleClient();
    TEST_ASSERT_EQUAL_STRING("/late", p.path.c_str());
    TEST_ASSERT_TRUE(c->closed);

    auto d = p.connect();
    setMillis(1000);
    p.srv.handleClient();
    setMillis(1000 + HttpServer::WAIT_MS);
    p.srv.handleClient();
    TEST_ASSERT_TRUE(d->closed);
    TEST_ASSERT_TRUE(d->tx.empty());
}

void test_connection_closed_without_request() {
    Probe p;
    auto c = p.connect();
    c->open = false;
    p.srv.handleClient();                    // dropped at accept
    TEST_ASSERT_TRUE(c->tx.empty());
    TEST_ASSERT_EQUAL(0, p.calls);
    p.b.send("GET /after HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("/after", p.path.c_str());

    // Closes after connecting, before the request
    auto d = p.connect();
    p.srv.handleClient();
    d->open = false;
    p.srv.handleClient();
    TEST_ASSERT_TRUE(d->closed);
}

// Browser preconnect: another connection with a request, or a full queue,
// makes the idle one yield after YIELD_MS
void test_idle_connection_yields_to_another() {
    Probe p;
    setMillis(0);
    auto idle = p.connect();
    p.srv.handleClient();
    auto real = p.connect("GET /real HTTP/1.1\r\n\r\n");
    setMillis(HttpServer::YIELD_MS);
    p.srv.handleClient();
    TEST_ASSERT_FALSE(idle->closed);
    setMillis(HttpServer::YIELD_MS + 1);
    p.srv.handleClient();
    TEST_ASSERT_TRUE(idle->closed);
    p.srv.handleClient();
    TEST_ASSERT_EQUAL_STRING("/real", p.path.c_str());

    auto idle2 = p.connect();
    p.srv.handleClient();
    p.connect();                             // no data, but the queue is full
    MockServer::maxPending = true;
    setMillis(millis() + HttpServer::YIELD_MS + 1);
    p.srv.handleClient();
    TEST_ASSERT_TRUE(idle2->closed);
}

// ── response ─────────────────────────────────────────────────────────────────

// A page in pieces: no Content-Length, ends on close; larger than the buffer
void test_page_in_pieces() {
    Probe p;
    std::string big(700, 'z');
    p.reply = [&](Probe& q) {
        q.srv.sendHeader("Cache-Control", "no-store");
        q.srv.setContentLength(HttpServer::UNKNOWN_LENGTH);
        q.srv.send(201, "text/html", "");
        q.srv.sendContent_P(PSTR("<p>"));
        q.srv.sendContent(String(big.c_str()));
        q.srv.sendContent("");
        q.srv.sendContent("</p>");
    };
    p.b.send("GET / HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL(201, p.b.code);
    TEST_ASSERT_EQUAL_STRING("no-store", p.b.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL(0, (int)p.b.headers.count("Content-Length"));
    TEST_ASSERT_TRUE(p.b.out == "<p>" + big + "</p>");
    TEST_ASSERT_TRUE(p.b.conn->writes >= 3);   // the buffer flushed halfway
    TEST_ASSERT_TRUE(p.b.conn->closed);
}

void test_body_from_flash() {
    Probe p;
    static const char DATA[] PROGMEM = "\x1f\x8b binario";
    p.reply = [](Probe& q) {
        q.srv.sendHeader("Content-Encoding", "gzip");
        q.srv.send_P(200, "image/x", DATA, sizeof(DATA) - 1);
    };
    p.b.send("GET / HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("image/x", p.b.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("gzip", p.b.headers["Content-Encoding"].c_str());
    TEST_ASSERT_EQUAL_STRING(std::to_string(sizeof(DATA) - 1).c_str(),
                             p.b.headers["Content-Length"].c_str());
    TEST_ASSERT_TRUE(p.b.out == std::string(DATA, sizeof(DATA) - 1));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_get_with_query_and_sized_response);
    RUN_TEST(test_post_urlencoded_and_plain);
    RUN_TEST(test_body_that_is_not_a_form);
    RUN_TEST(test_decoding_and_similar_names);
    RUN_TEST(test_headers);
    RUN_TEST(test_unknown_method_reaches_handler);
    RUN_TEST(test_malformed_request);
    RUN_TEST(test_path_too_long);
    RUN_TEST(test_body_too_large_or_negative);
    RUN_TEST(test_incomplete_body);
    RUN_TEST(test_idle_connection_waits_and_gives_up);
    RUN_TEST(test_connection_closed_without_request);
    RUN_TEST(test_idle_connection_yields_to_another);
    RUN_TEST(test_page_in_pieces);
    RUN_TEST(test_body_from_flash);
    return UNITY_END();
}
