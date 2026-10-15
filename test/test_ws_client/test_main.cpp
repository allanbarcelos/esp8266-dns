// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "WsClient.h"
#include "../ws_frames.h"

void setUp()    { resetMocks(); }
void tearDown() {}

struct Rig {
    MockConn   conn;
    WiFiClient c{&conn};
    WsClient   ws{c};
};

static const char* OK101 = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n";

// ── handshake ────────────────────────────────────────────────────────────────

void test_handshake_request_and_101() {
    Rig r;
    r.conn.rx = OK101;
    TEST_ASSERT_EQUAL(101, r.ws.handshake("relay.example", "/.relay", "X-A: 1\r\n"));
    const std::string& tx = r.conn.tx;
    TEST_ASSERT_TRUE(tx.rfind("GET /.relay HTTP/1.1\r\n", 0) == 0);
    TEST_ASSERT_TRUE(contains(tx, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"));
    TEST_ASSERT_TRUE(contains(tx, "\r\nSec-WebSocket-Version: 13\r\n"));
    TEST_ASSERT_TRUE(contains(tx, "\r\nHost: relay.example\r\nX-A: 1\r\n\r\n"));
    // The key: 16 bytes in base64
    size_t k = tx.find("Sec-WebSocket-Key: ") + 19;
    std::string key = tx.substr(k, tx.find("\r\n", k) - k);
    TEST_ASSERT_EQUAL(24, (int)key.size());
    TEST_ASSERT_EQUAL_STRING("==", key.substr(22).c_str());
    TEST_ASSERT_EQUAL(22, (int)strspn(key.c_str(),
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"));
}

void test_handshake_refused_or_broken() {
    Rig a;
    a.conn.rx = "HTTP/1.1 426 Upgrade Required\r\nContent-Length: 0\r\n\r\n";
    TEST_ASSERT_EQUAL(426, a.ws.handshake("h", "/", ""));

    Rig b;
    b.conn.rx = "HTTP/1.0 101 x\r\n\r\n";         // not HTTP/1.1
    TEST_ASSERT_EQUAL(-1, b.ws.handshake("h", "/", ""));

    Rig c;
    c.conn.rx = "HTTP/1.1\r\n\r\n";                // no status
    TEST_ASSERT_EQUAL(-1, c.ws.handshake("h", "/", ""));

    Rig d;
    d.conn.rx = "HTTP/1.1 101 x\r\nUpgrade: web";  // headers cut
    d.conn.open = false;
    TEST_ASSERT_EQUAL(-1, d.ws.handshake("h", "/", ""));

    Rig e;                                         // a long line is cut, not overflowed
    e.conn.rx = "HTTP/1.1 101 " + std::string(200, 'x') + "\r\n\r\n";
    TEST_ASSERT_EQUAL(101, e.ws.handshake("h", "/", ""));

    Rig f;                                         // silence: gives up after BYTE_MS
    TEST_ASSERT_EQUAL(-1, f.ws.handshake("h", "/", ""));
    TEST_ASSERT_TRUE(millis() >= WsClient::BYTE_MS);
}

// ── sending ──────────────────────────────────────────────────────────────────

void test_send_masks_short_and_long_frames() {
    Rig r;
    r.ws.send(WsClient::TEXT, "ping", 4);
    std::string head = "R\x00\x07";
    std::string body(300, 'b');
    r.ws.send(WsClient::BINARY, head.data(), head.size(), body.data(), body.size());

    size_t at = 0;
    ClientFrame f1 = takeClientFrame(r.conn.tx, at);
    TEST_ASSERT_TRUE(f1.ok && f1.masked);
    TEST_ASSERT_EQUAL(WsClient::TEXT, f1.op);
    TEST_ASSERT_EQUAL_STRING("ping", f1.payload.c_str());
    ClientFrame f2 = takeClientFrame(r.conn.tx, at);
    TEST_ASSERT_TRUE(f2.ok && f2.masked);
    TEST_ASSERT_EQUAL(WsClient::BINARY, f2.op);
    TEST_ASSERT_TRUE(f2.payload == head + body);
    TEST_ASSERT_EQUAL(r.conn.tx.size(), at);
    // The mask is not zero: the bytes on the wire differ from the payload
    TEST_ASSERT_FALSE(contains(r.conn.tx, "ping"));
}

void test_send_empty_frame() {
    Rig r;
    r.ws.send(WsClient::PONG, nullptr, 0);
    size_t at = 0;
    ClientFrame f = takeClientFrame(r.conn.tx, at);
    TEST_ASSERT_TRUE(f.ok);
    TEST_ASSERT_EQUAL(WsClient::PONG, f.op);
    TEST_ASSERT_EQUAL(0, (int)f.payload.size());
}

// ── receiving ────────────────────────────────────────────────────────────────

void test_frames_of_each_length_form() {
    Rig r;
    std::string mid(200, 'm');
    std::string big(70000 % WsClient::MAX_FRAME, 'g');
    r.conn.rx = serverFrame(WsClient::TEXT, "hello") + serverFrame(WsClient::BINARY, mid) +
                serverFrame(WsClient::BINARY, big, true);

    TEST_ASSERT_TRUE(r.ws.frame());
    TEST_ASSERT_EQUAL(WsClient::TEXT, r.ws.op());
    char buf[8];
    TEST_ASSERT_EQUAL(5, (int)r.ws.read(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("hello", buf);
    TEST_ASSERT_EQUAL(-1, r.ws.next());           // the frame is over

    TEST_ASSERT_TRUE(r.ws.frame());
    TEST_ASSERT_EQUAL(200u, r.ws.left());
    TEST_ASSERT_EQUAL(3, (int)r.ws.read(buf, 4)); // the rest skipped
    TEST_ASSERT_EQUAL_STRING("mmm", buf);
    TEST_ASSERT_EQUAL(0u, r.ws.left());

    TEST_ASSERT_TRUE(r.ws.frame());               // 64-bit length form
    TEST_ASSERT_EQUAL((uint32_t)big.size(), r.ws.left());
    r.ws.skip();
    TEST_ASSERT_EQUAL(0u, r.ws.left());
    TEST_ASSERT_FALSE(r.ws.error());
    TEST_ASSERT_FALSE(r.ws.frame());               // nothing more yet
    TEST_ASSERT_FALSE(r.ws.error());
}

void test_partial_header_waits() {
    Rig r;
    r.conn.rx = std::string("\x82", 1);
    TEST_ASSERT_FALSE(r.ws.frame());
    TEST_ASSERT_FALSE(r.ws.error());
    r.conn.rx += std::string("\x00", 1);
    TEST_ASSERT_TRUE(r.ws.frame());
    TEST_ASSERT_EQUAL(0u, r.ws.left());
}

void test_protocol_errors() {
    struct Case { std::string bytes; };
    const Case cases[] = {
        {std::string("\x02\x01x", 3)},                    // not FIN (fragmented)
        {std::string("\xC2\x01x", 3)},                    // reserved bit
        {std::string("\x82\x81", 2) + "abcdx"},           // masked by the server
        {std::string("\x80\x01x", 3)},                    // continuation
        {std::string("\x82\x7E\x20\x01", 4)},             // 8193 bytes: too long
        {std::string("\x82\x7F\x00\x00\x00\x01\x00\x00\x00\x00", 10)},   // 4 GB
        {std::string("\x82\x7E\x00", 3)},                 // length cut
    };
    for (const Case& c : cases) {
        Rig r;
        r.conn.rx = c.bytes;
        r.conn.open = false;
        TEST_ASSERT_FALSE(r.ws.frame());
        TEST_ASSERT_TRUE(r.ws.error());
    }
}

void test_payload_cut_is_an_error() {
    Rig r;
    r.conn.rx = serverFrame(WsClient::BINARY, "abcdef").substr(0, 5);
    r.conn.open = false;
    TEST_ASSERT_TRUE(r.ws.frame());
    char buf[8];
    TEST_ASSERT_EQUAL(3, (int)r.ws.read(buf, sizeof(buf)));
    TEST_ASSERT_TRUE(r.ws.error());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handshake_request_and_101);
    RUN_TEST(test_handshake_refused_or_broken);
    RUN_TEST(test_send_masks_short_and_long_frames);
    RUN_TEST(test_send_empty_frame);
    RUN_TEST(test_frames_of_each_length_form);
    RUN_TEST(test_partial_header_waits);
    RUN_TEST(test_protocol_errors);
    RUN_TEST(test_payload_cut_is_an_error);
    return UNITY_END();
}
