// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "../ws_frames.h"
#include "Clock.h"
#include "Config.h"
#include "RelayService.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

static const char* SECRET = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
static const char* OK101 = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n";

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    strlcpy(cfg.relay_host, "esp-pump.example.com", sizeof(cfg.relay_host));
    strlcpy(cfg.relay_secret, SECRET, sizeof(cfg.relay_secret));
    cfg.relay_ver = RelayService::PROTOCOL;
    MockHTTP::keepOpen = true;
}
void tearDown() {}

// A server that answers each request with its path and the client's address
struct Rig {
    Clock        clock{fakeTime};
    HttpServer   http{80};
    IoTService   iot{cfg, logger};
    RelayService relay;
    std::string  lastPath, lastBody;
    uint32_t     lastIp = 0;
    size_t       at = 0;   // frames already read from the wire

    explicit Rig(bool withIot = false) : relay(cfg, logger, clock, http, withIot ? &iot : nullptr) {
        http.begin(&Rig::handle, this);
    }

    static void handle(void* self) {
        Rig& r = *static_cast<Rig*>(self);
        r.lastPath = r.http.path();
        r.lastBody = r.http.arg("plain").c_str();
        r.lastIp = r.http.remoteIP();
        r.http.send(200, "text/plain", String("at ") + r.http.path());
    }

    void open(unsigned long t = 0) {
        MockHTTP::rawHttp = OK101;
        relay.tick(t);
    }

    void inject(const std::string& bytes) { MockTLS::live->inject(bytes); }

    ClientFrame next() { return takeClientFrame(MockTLS::wire, at); }

    // Frames until the wire runs out
    std::vector<ClientFrame> drain() {
        std::vector<ClientFrame> out;
        for (ClientFrame f = next(); f.ok; f = next()) out.push_back(f);
        return out;
    }
};

static std::string req(uint16_t id, const std::string& http, const uint8_t ip[4] = nullptr) {
    static const uint8_t ZERO[4] = {0, 0, 0, 0};
    const uint8_t* a = ip ? ip : ZERO;
    std::string p = "R";
    p += (char)(id >> 8);
    p += (char)id;
    p.append(reinterpret_cast<const char*>(a), 4);
    return serverFrame(WsClient::BINARY, p + http);
}

static std::string hmacHex(const char* key, const char* msg) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha1_vtable, key, strlen(key));
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, msg, strlen(msg));
    uint8_t d[20];
    br_hmac_out(&hc, d);
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return hex;
}

// ── connecting ───────────────────────────────────────────────────────────────

void test_off_until_deployed_for_this_version() {
    Rig r;
    cfg.relay_ver = RelayService::PROTOCOL + 1;
    TEST_ASSERT_FALSE(r.relay.configured());
    TEST_ASSERT_EQUAL_STRING("off", r.relay.status());
    r.relay.tick(0);
    TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);

    cfg.relay_ver = RelayService::PROTOCOL;
    cfg.relay_secret[0] = '\0';
    TEST_ASSERT_FALSE(r.relay.configured());
    strlcpy(cfg.relay_secret, SECRET, sizeof(cfg.relay_secret));
    cfg.relay_host[0] = '\0';
    TEST_ASSERT_FALSE(r.relay.configured());
}

void test_connects_with_secret_and_checked_certificate() {
    Rig r;
    TEST_ASSERT_EQUAL_STRING("connecting", r.relay.status());
    r.open();
    TEST_ASSERT_TRUE(r.relay.up());
    TEST_ASSERT_EQUAL_STRING("connected", r.relay.status());
    TEST_ASSERT_TRUE(logHas(logger, "relay: connected"));
    const std::string& h = MockHTTP::lastRaw;
    TEST_ASSERT_TRUE(h.rfind("GET /.relay HTTP/1.1\r\n", 0) == 0);
    TEST_ASSERT_TRUE(contains(h, "\r\nHost: esp-pump.example.com\r\n"));
    TEST_ASSERT_TRUE(contains(h, (std::string("\r\nAuthorization: Bearer ") + SECRET + "\r\n").c_str()));
    TEST_ASSERT_TRUE(contains(h, "\r\nX-Relay-Version: 1\r\n"));
    TEST_ASSERT_TRUE(cloudflareTrust(MockTLS::lastTrust));
    TEST_ASSERT_EQUAL((long)fakeNow, (long)MockTLS::lastTime);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);

    // Hello first; no state without a sensor or actuator
    auto frames = r.drain();
    TEST_ASSERT_EQUAL(1, (int)frames.size());
    TEST_ASSERT_EQUAL(WsClient::BINARY, frames[0].op);
    TEST_ASSERT_EQUAL_STRING("H{\"v\":1,\"fw\":\"dev\",\"api\":\"\"}", frames[0].payload.c_str());
}

void test_hello_carries_a_keyed_hash_of_the_api_token() {
    strlcpy(cfg.api_token, "0123456789abcdef0123456789abcdef", sizeof(cfg.api_token));
    Rig r;
    r.open();
    std::string want = "H{\"v\":1,\"fw\":\"dev\",\"api\":\"" + hmacHex(SECRET, cfg.api_token) + "\"}";
    TEST_ASSERT_EQUAL_STRING(want.c_str(), r.next().payload.c_str());
    TEST_ASSERT_FALSE(contains(MockTLS::wire, cfg.api_token));

    // A new token goes up in a new hello, within STATE_MS
    strlcpy(cfg.api_token, "ffff456789abcdef0123456789abcdef", sizeof(cfg.api_token));
    r.relay.tick(RelayService::STATE_MS);
    want = "H{\"v\":1,\"fw\":\"dev\",\"api\":\"" + hmacHex(SECRET, cfg.api_token) + "\"}";
    TEST_ASSERT_EQUAL_STRING(want.c_str(), r.next().payload.c_str());
    r.relay.tick(2 * RelayService::STATE_MS);
    TEST_ASSERT_FALSE(r.next().ok);
}

void test_waits_for_the_clock() {
    fakeNow = 0;
    Rig r;
    r.relay.tick(0);
    TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    r.relay.tick(RelayService::RECONNECT_MS - 1);
    TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    r.open(RelayService::RECONNECT_MS);
    TEST_ASSERT_TRUE(r.relay.up());
}

void test_failures_back_off_up_to_five_minutes() {
    Rig r;
    MockTLS::failConnect = true;
    unsigned long t = 0;
    unsigned long waits[] = {5, 10, 20, 40, 80, 160, 300, 300};
    for (unsigned long w : waits) {
        int before = MockTLS::connectCalls;
        r.relay.tick(t);
        TEST_ASSERT_EQUAL(before + 1, MockTLS::connectCalls);
        r.relay.tick(t + w * 1000 - 1);
        TEST_ASSERT_EQUAL(before + 1, MockTLS::connectCalls);
        t += w * 1000;
    }
    TEST_ASSERT_TRUE(logHas(logger, "relay: no connection"));
    TEST_ASSERT_TRUE(MockTLS::stopCalls >= 8);   // each failure closed its session

    // Settings changed: right away, from the first wait again
    MockTLS::failConnect = false;
    r.relay.reconnect();
    r.open(t);
    TEST_ASSERT_TRUE(r.relay.up());
}

void test_not_enough_memory() {
    Rig r;
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    r.relay.tick(0);
    TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    TEST_ASSERT_TRUE(logHas(logger, "relay: not enough memory"));
}

void test_prepare_fails() {
    MockTLS::nullOwned = true;
    Rig r;
    r.relay.tick(0);
    TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    TEST_ASSERT_TRUE(logHas(logger, "relay: no connection"));
}

void test_refused_and_outdated() {
    Rig r;
    MockHTTP::rawHttp = "HTTP/1.1 401 Unauthorized\r\n\r\n";
    r.relay.tick(0);
    TEST_ASSERT_FALSE(r.relay.up());
    TEST_ASSERT_FALSE(r.relay.outdated());
    TEST_ASSERT_TRUE(logHas(logger, "relay: refused (HTTP 401)"));

    MockHTTP::rawHttp = "HTTP/1.1 426 Upgrade Required\r\n\r\n";
    r.relay.tick(5000);
    TEST_ASSERT_TRUE(r.relay.outdated());
    TEST_ASSERT_EQUAL_STRING("outdated", r.relay.status());

    r.relay.reconnect();
    TEST_ASSERT_FALSE(r.relay.outdated());
}

void test_hold_and_wifi_close_the_connection() {
    Rig r;
    r.open();
    r.relay.hold(true);
    TEST_ASSERT_FALSE(r.relay.up());
    TEST_ASSERT_EQUAL_STRING("paused", r.relay.status());
    r.relay.tick(10);
    TEST_ASSERT_EQUAL(1, MockTLS::connectCalls);
    r.relay.hold(false);
    r.open(20);
    TEST_ASSERT_TRUE(r.relay.up());
    TEST_ASSERT_EQUAL(2, MockTLS::connectCalls);

    WiFi._status = WL_DISCONNECTED;
    r.relay.tick(30);
    TEST_ASSERT_FALSE(r.relay.up());
}

// ── requests ─────────────────────────────────────────────────────────────────

void test_request_answered_through_the_server() {
    Rig r;
    r.open();
    r.drain();
    const uint8_t ip[4] = {203, 0, 113, 9};
    r.inject(req(0x0102, "POST /x HTTP/1.1\r\nHost: esp-pump.example.com\r\nContent-Length: 3\r\n\r\nabc", ip));
    r.relay.tick(100);
    TEST_ASSERT_EQUAL_STRING("/x", r.lastPath.c_str());
    TEST_ASSERT_EQUAL_STRING("abc", r.lastBody.c_str());
    TEST_ASSERT_EQUAL_HEX32(0x097100CB, r.lastIp);   // 203.0.113.9, as IPAddress keeps it

    std::string answer;
    auto frames = r.drain();
    TEST_ASSERT_TRUE(frames.size() >= 2);
    for (size_t i = 0; i + 1 < frames.size(); i++) {
        TEST_ASSERT_EQUAL(WsClient::BINARY, frames[i].op);
        TEST_ASSERT_EQUAL_STRING("D\x01\x02", frames[i].payload.substr(0, 3).c_str());
        answer += frames[i].payload.substr(3);
    }
    TEST_ASSERT_TRUE(frames.back().payload == std::string("E\x01\x02"));
    TEST_ASSERT_TRUE(answer.rfind("HTTP/1.1 200 \r\n", 0) == 0);
    TEST_ASSERT_TRUE(contains(answer, "\r\n\r\nat /x"));
}

void test_bytes_past_the_request_are_skipped() {
    Rig r;
    r.open();
    r.drain();
    r.inject(req(7, "GET /a HTTP/1.1\r\n\r\nleftover") + req(8, "GET /b HTTP/1.1\r\n\r\n"));
    r.relay.tick(100);
    TEST_ASSERT_EQUAL_STRING("/b", r.lastPath.c_str());
    int ends = 0;
    for (auto& f : r.drain()) ends += f.payload[0] == 'E';
    TEST_ASSERT_EQUAL(2, ends);
}

void test_cut_request_header_is_dropped() {
    Rig r;
    r.open();
    r.drain();
    r.inject(serverFrame(WsClient::BINARY, std::string("R\x00\x01", 3)));
    r.relay.tick(100);
    TEST_ASSERT_EQUAL_STRING("", r.lastPath.c_str());
    TEST_ASSERT_FALSE(r.next().ok);
    TEST_ASSERT_TRUE(r.relay.up());
}

void test_incomplete_request_gets_400() {
    Rig r;
    r.open();
    r.drain();
    r.inject(req(3, "GET /a HTTP/1.1\r\n"));
    r.relay.tick(100);
    std::string answer;
    for (auto& f : r.drain())
        if (f.payload[0] == 'D') answer += f.payload.substr(3);
    TEST_ASSERT_TRUE(answer.rfind("HTTP/1.1 400 ", 0) == 0);
}

// The payload stops arriving halfway: the request ends there (400) and the
// connection, now out of step, is dropped
void test_payload_that_stops_arriving() {
    Rig r;
    r.open();
    r.drain();
    r.inject(req(3, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n").substr(0, 15));
    r.relay.tick(100);
    TEST_ASSERT_FALSE(r.relay.up());
    TEST_ASSERT_TRUE(logHas(logger, "relay: connection lost"));

    // The same with the connection closing: no waiting for the rest
    Rig s;
    s.open();
    s.inject(req(4, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n").substr(0, 15));
    MockHTTP::keepOpen = false;
    unsigned long before = millis();
    s.relay.tick(200);
    TEST_ASSERT_TRUE(millis() - before < HttpServer::READ_MS);
    TEST_ASSERT_FALSE(s.relay.up());
}

// ── control and other frames ─────────────────────────────────────────────────

void test_ping_pong_and_other_frames() {
    Rig r;
    r.open();
    r.drain();
    r.inject(serverFrame(WsClient::PING, "abc") + serverFrame(WsClient::TEXT, "pong") +
             serverFrame(WsClient::BINARY, "Zzz") + serverFrame(WsClient::BINARY, ""));
    r.relay.tick(100);
    auto frames = r.drain();
    TEST_ASSERT_EQUAL(1, (int)frames.size());
    TEST_ASSERT_EQUAL(WsClient::PONG, frames[0].op);
    TEST_ASSERT_EQUAL_STRING("abc", frames[0].payload.c_str());
    TEST_ASSERT_TRUE(r.relay.up());
}

void test_a_few_frames_per_tick() {
    Rig r;
    r.open();
    r.drain();
    for (int i = 0; i < RelayService::FRAMES_PER_TICK + 1; i++) r.inject(serverFrame(WsClient::PING, "p"));
    r.relay.tick(100);
    TEST_ASSERT_EQUAL(RelayService::FRAMES_PER_TICK, (int)r.drain().size());
    r.relay.tick(101);
    TEST_ASSERT_EQUAL(1, (int)r.drain().size());
}

void test_keepalive_ping() {
    Rig r;
    r.open();
    r.drain();
    r.relay.tick(RelayService::PING_MS - 1);
    TEST_ASSERT_FALSE(r.next().ok);
    r.relay.tick(RelayService::PING_MS);
    ClientFrame f = r.next();
    TEST_ASSERT_EQUAL(WsClient::TEXT, f.op);
    TEST_ASSERT_EQUAL_STRING("ping", f.payload.c_str());
}

void test_close_from_the_worker() {
    Rig r;
    r.open();
    r.inject(serverFrame(WsClient::CLOSE, std::string("\x03\xe8", 2)));
    r.relay.tick(100);
    TEST_ASSERT_FALSE(r.relay.up());
    TEST_ASSERT_TRUE(logHas(logger, "relay: closed by the Worker"));
    r.relay.tick(100 + RelayService::RECONNECT_MS - 1);
    TEST_ASSERT_EQUAL(1, MockTLS::connectCalls);
    r.open(100 + RelayService::RECONNECT_MS);
    TEST_ASSERT_EQUAL(2, MockTLS::connectCalls);
}

void test_protocol_error_drops_the_connection() {
    Rig r;
    r.open();
    r.inject(std::string("\x82\x81", 2) + "abcdx");   // masked by the server
    r.relay.tick(100);
    TEST_ASSERT_FALSE(r.relay.up());
    TEST_ASSERT_TRUE(logHas(logger, "relay: connection lost"));
}

void test_connection_closed_by_the_network() {
    Rig r;
    r.open();
    MockHTTP::keepOpen = false;
    r.relay.tick(100);
    TEST_ASSERT_FALSE(r.relay.up());
    TEST_ASSERT_TRUE(logHas(logger, "relay: connection lost"));
}

void test_silent_worker_is_dropped() {
    Rig r;
    r.open();
    r.relay.tick(RelayService::IDLE_MS - 1);
    TEST_ASSERT_TRUE(r.relay.up());
    r.relay.tick(RelayService::IDLE_MS);
    TEST_ASSERT_FALSE(r.relay.up());
    TEST_ASSERT_TRUE(logHas(logger, "relay: stalled"));
}

// ── state ────────────────────────────────────────────────────────────────────

void test_state_goes_up_on_change_and_every_five_minutes() {
    strlcpy(cfg.iot.actuator, "relay", sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;
    Rig r(true);
    r.iot.begin();
    r.open();
    auto frames = r.drain();
    TEST_ASSERT_EQUAL(2, (int)frames.size());
    char json[400];
    r.iot.stateJson(json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING((std::string("S") + json).c_str(), frames[1].payload.c_str());

    // The same state is not sent again
    unsigned long t = RelayService::STATE_MS;
    r.relay.tick(t);
    TEST_ASSERT_FALSE(r.next().ok);

    // A change goes up within STATE_MS
    r.iot.command("on", t);
    r.relay.tick(t + RelayService::STATE_MS - 1);
    TEST_ASSERT_FALSE(r.next().ok);
    r.relay.tick(t + RelayService::STATE_MS);
    ClientFrame f = r.next();
    TEST_ASSERT_TRUE(contains(f.payload, "\"on\":true"));

    // Every TELE_MS even without a change; the Worker answers the pings meanwhile
    unsigned long sent = t + RelayService::STATE_MS;
    int states = 0;
    for (unsigned long now = sent; now <= sent + RelayService::TELE_MS; now += RelayService::STATE_MS) {
        if (now % RelayService::PING_MS == 0) r.inject(serverFrame(WsClient::TEXT, "pong"));
        r.relay.tick(now);
        for (auto& g : r.drain()) states += g.payload[0] == 'S';
    }
    TEST_ASSERT_TRUE(r.relay.up());
    TEST_ASSERT_EQUAL(1, states);
}

// Only with a sensor or an actuator (the API answers 404 otherwise)
void test_state_needs_a_sensor_or_actuator() {
    Rig none(true);
    none.iot.begin();
    none.open();
    TEST_ASSERT_EQUAL(1, (int)none.drain().size());   // hello only
    none.relay.hold(true);

    strlcpy(cfg.iot.sensor, "digital", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    Rig sensor(true);
    sensor.iot.begin();
    sensor.at = MockTLS::wire.size();
    sensor.open();
    auto frames = sensor.drain();
    TEST_ASSERT_EQUAL(2, (int)frames.size());
    TEST_ASSERT_EQUAL('S', frames[1].payload[0]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_off_until_deployed_for_this_version);
    RUN_TEST(test_connects_with_secret_and_checked_certificate);
    RUN_TEST(test_hello_carries_a_keyed_hash_of_the_api_token);
    RUN_TEST(test_waits_for_the_clock);
    RUN_TEST(test_failures_back_off_up_to_five_minutes);
    RUN_TEST(test_not_enough_memory);
    RUN_TEST(test_prepare_fails);
    RUN_TEST(test_refused_and_outdated);
    RUN_TEST(test_hold_and_wifi_close_the_connection);
    RUN_TEST(test_request_answered_through_the_server);
    RUN_TEST(test_bytes_past_the_request_are_skipped);
    RUN_TEST(test_cut_request_header_is_dropped);
    RUN_TEST(test_incomplete_request_gets_400);
    RUN_TEST(test_payload_that_stops_arriving);
    RUN_TEST(test_ping_pong_and_other_frames);
    RUN_TEST(test_a_few_frames_per_tick);
    RUN_TEST(test_keepalive_ping);
    RUN_TEST(test_close_from_the_worker);
    RUN_TEST(test_protocol_error_drops_the_connection);
    RUN_TEST(test_connection_closed_by_the_network);
    RUN_TEST(test_silent_worker_is_dropped);
    RUN_TEST(test_state_goes_up_on_change_and_every_five_minutes);
    RUN_TEST(test_state_needs_a_sensor_or_actuator);
    return UNITY_END();
}
