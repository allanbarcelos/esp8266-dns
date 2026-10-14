// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Clock.h"
#include "Config.h"
#include "NtfyNotifier.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    strlcpy(cfg.ntfy_topic, "my-topic", sizeof(cfg.ntfy_topic));
}
void tearDown() {}

struct Fix {
    Clock        clock{fakeTime};
    NtfyNotifier ntfy{cfg, logger, clock};
};

void test_no_post_without_topic() {
    cfg.ntfy_topic[0] = '\0';
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    f.ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

// The IP comes from PublicIPResolver: with no known IP, nothing to publish
void test_without_ip_does_not_post() {
    Fix f;
    f.ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_posts_at_boot_without_cloudflare() {
    TEST_ASSERT_EQUAL_STRING("", cfg.cf_token);
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    MockHTTP::push(200);
    f.ntfy.tick(0);

    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/my-topic", MockHTTP::requests[0].url.c_str());
    TEST_ASSERT_EQUAL_STRING("POST", MockHTTP::requests[0].method.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "[ip] 1.2.3.4"));
    TEST_ASSERT_TRUE(logHas(logger, "ntfy sent (200)"));
    for (auto& r : MockHTTP::requests) {
        TEST_ASSERT_TRUE(cloudflareTrust(r.trust));
        TEST_ASSERT_EQUAL((long)fakeNow, r.x509Time);
        TEST_ASSERT_FALSE(r.insecure);
    }
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
}

void test_does_not_repost_same_ip() {
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    MockHTTP::push(200);
    f.ntfy.tick(0);
    f.ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
}

void test_reposts_when_ip_changes() {
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    MockHTTP::push(200);
    f.ntfy.tick(0);
    strlcpy(cfg.publicIP, "5.6.7.8", sizeof(cfg.publicIP));
    MockHTTP::push(200);
    f.ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[1].payload, "[ip] 5.6.7.8"));
}

void test_failure_waits_interval_to_retry() {
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    MockHTTP::push(500);
    f.ntfy.tick(1000);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy error: 500"));

    f.ntfy.tick(1000 + NtfyNotifier::RETRY_INTERVAL - 1);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());

    MockHTTP::push(200);
    f.ntfy.tick(1000 + NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy sent (200)"));
}

void test_connection_error() {
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    MockHTTP::failNextBegin();
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: connection failed"));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::requests.size());
}

void test_negative_code() {
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    MockHTTP::push(-1);
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy error: -1"));
}

void test_not_enough_memory_does_not_post() {
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: not enough memory"));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_tls_unavailable_does_not_post() {
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    MockTLS::nullOwned = true;
    MockTLS::bufIn = 0;
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: TLS unavailable"));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(0, MockTLS::bufIn);
}

void test_iot_without_reading_still_posts_ip() {
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    MockHTTP::push(200, "ok");
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "[ip] 1.2.3.4"));
}

void test_sensor_on_the_same_topic() {
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    strlcpy(cfg.iot.sensor, "digital", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    cfg.iot.notify = true;
    mock::pinIn[2] = HIGH;
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(iot.pending());

    MockHTTP::push(200, "ok");
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("POST", MockHTTP::requests[0].method.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "[sensor] active"));
    TEST_ASSERT_FALSE(iot.pending());

    mock::pinIn[2] = LOW;
    iot.tick(IoTService::DEBOUNCE_MS + 1);
    iot.tick(IoTService::DEBOUNCE_MS + 1 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(iot.pending());
    ntfy.tick(IoTService::DEBOUNCE_MS + 1 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());

    MockHTTP::push(500, "x");
    ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_TRUE(iot.pending());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy error: 500"));
}

void test_validates_topic() {
    TEST_ASSERT_TRUE(NtfyNotifier::isValidTopic("abc_DEF-123"));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic(""));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic(String(std::string(64, 'a'))));
    TEST_ASSERT_TRUE(NtfyNotifier::isValidTopic(String(std::string(63, 'a'))));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic("a/b"));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic("a b"));
}

// ── Commands over ntfy (continuous stream) ───────────────────────────────────

static const char* OK_HDR = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n\r\n";

struct Cmd {
    Clock            clock{fakeTime};
    IoTService       iot{cfg, logger};
    NtfyNotifier     ntfy{cfg, logger, clock, &iot};

    Cmd() {
        cfg.ntfy_topic[0] = '\0';               // commands only: nothing to publish
        strlcpy(cfg.ntfy_cmd, "cmd-abc", sizeof(cfg.ntfy_cmd));
        strlcpy(cfg.iot.actuator, "relay", sizeof(cfg.iot.actuator));
        cfg.iot.actuatorPin = 0;
        iot.begin();
        MockHTTP::keepOpen = true;
    }

    // Connects and reads the status and headers
    void open(unsigned long t = 0) {
        MockHTTP::rawHttp = OK_HDR;
        ntfy.tick(t);
        ntfy.tick(t + 1);
    }

    void send(const std::string& line) { MockTLS::live->inject(line + "\r\n"); }
};

static std::string message(const char* id, const char* text, long age = 0) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%s\",\"time\":%ld,\"event\":\"message\",\"topic\":\"cmd-abc\","
             "\"message\":\"%s\"}", id, (long)fakeNow - age, text);
    return buf;
}

void test_commands_connect_validating_certificate() {
    Cmd c;
    TEST_ASSERT_EQUAL_STRING("connecting", c.ntfy.commandStatus());
    MockHTTP::rawHttp = OK_HDR;
    c.ntfy.tick(0);
    TEST_ASSERT_TRUE(contains(MockHTTP::lastRaw,
        "GET /cmd-abc/json HTTP/1.0\r\nHost: ntfy.sh\r\nUser-Agent: ESP8266\r\n\r\n"));
    TEST_ASSERT_FALSE(contains(MockHTTP::lastRaw, "Authorization"));
    TEST_ASSERT_TRUE(cloudflareTrust(MockTLS::lastTrust));
    TEST_ASSERT_EQUAL((long)fakeNow, (long)MockTLS::lastTime);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
    TEST_ASSERT_EQUAL(NtfyNotifier::CONNECT_MS, MockTLS::lastTimeoutMs);

    c.ntfy.tick(1);
    TEST_ASSERT_EQUAL_STRING("connected", c.ntfy.commandStatus());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: commands connected"));

    c.send("{\"id\":\"o1\",\"time\":1,\"event\":\"open\",\"topic\":\"cmd-abc\"}");
    c.send("{\"event\":\"keepalive\"}");
    c.send("junk");
    c.send(message("m1", "on"));
    c.ntfy.tick(2);
    TEST_ASSERT_TRUE(c.iot.actuatorOn());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: command \"on\""));

    c.send(message("m2", "OFF"));
    c.ntfy.tick(3);
    TEST_ASSERT_FALSE(c.iot.actuatorOn());
}

void test_commands_reconnect_asking_for_missed() {
    Cmd c;
    c.open();
    c.send(message("m7", "on"));
    c.ntfy.tick(10);

    // The server closed: waits and reopens with since=<last id>
    MockHTTP::keepOpen = false;
    c.ntfy.tick(20);
    TEST_ASSERT_TRUE(logHas(logger, "command stream closed"));
    MockHTTP::keepOpen = true;
    MockHTTP::rawHttp = OK_HDR;
    c.ntfy.tick(20 + NtfyNotifier::RECONNECT_MS - 1);
    TEST_ASSERT_EQUAL_STRING("connecting", c.ntfy.commandStatus());
    TEST_ASSERT_FALSE(contains(MockHTTP::lastRaw, "since"));
    c.ntfy.tick(20 + NtfyNotifier::RECONNECT_MS);
    TEST_ASSERT_TRUE(contains(MockHTTP::lastRaw, "GET /cmd-abc/json?since=m7 HTTP/1.0"));

    // Nothing (not even a keepalive) for a long time: reconnects
    unsigned long t = 20 + NtfyNotifier::RECONNECT_MS;
    c.ntfy.tick(t + 1);
    c.ntfy.tick(t + 1 + NtfyNotifier::IDLE_TIMEOUT);
    TEST_ASSERT_EQUAL_STRING("connected", c.ntfy.commandStatus());
    c.ntfy.tick(t + 2 + NtfyNotifier::IDLE_TIMEOUT);
    TEST_ASSERT_TRUE(logHas(logger, "command stream stalled"));
}

void test_commands_with_token_and_error_reply() {
    strlcpy(cfg.ntfy_token, "tk_secret", sizeof(cfg.ntfy_token));
    Cmd c;
    c.open();
    TEST_ASSERT_TRUE(contains(MockHTTP::lastRaw, "Authorization: Bearer tk_secret\r\n"));
}

void test_invalid_commands_reply_on_data_topic() {
    Cmd c;
    c.open();
    c.send(message("e1", "fly"));
    c.ntfy.tick(10);
    TEST_ASSERT_TRUE(logHas(logger, "[error] invalid command: fly"));
    c.send(message("e2", "50%"));
    c.ntfy.tick(11);
    TEST_ASSERT_TRUE(logHas(logger, "[error] command not valid for this actuator: 50%"));

    // With a data topic, the reply goes out and the stream closes for the post
    strlcpy(cfg.ntfy_topic, "data", sizeof(cfg.ntfy_topic));
    strlcpy(cfg.ntfy_token, "tk_x", sizeof(cfg.ntfy_token));
    MockHTTP::push(200, "ok");
    c.ntfy.tick(12);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "[error] command not valid"));
    bool auth = false;
    for (auto& h : MockHTTP::requests[0].headers)
        auth = auth || (h.first == "Authorization" && h.second == "Bearer tk_x");
    TEST_ASSERT_TRUE(auth);
    TEST_ASSERT_TRUE(cloudflareTrust(MockHTTP::requests[0].trust));
    TEST_ASSERT_EQUAL((long)fakeNow, MockHTTP::requests[0].x509Time);
    TEST_ASSERT_FALSE(MockHTTP::requests[0].insecure);
    TEST_ASSERT_FALSE(MockTLS::live && MockTLS::live->isConnected());

    // A failed reply retries after the short interval
    MockHTTP::rawHttp = OK_HDR;
    c.ntfy.tick(13);                                 // reconnects
    c.ntfy.tick(14);
    c.send(message("e3", "fly"));
    c.ntfy.tick(15);
    MockHTTP::push(500, "x");
    c.ntfy.tick(12 + NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
}

void test_commands_ignored_when_old_or_long() {
    Cmd c;
    c.open();
    c.send(message("v1", "on", NtfyNotifier::STALE_S + 1));
    c.ntfy.tick(10);
    TEST_ASSERT_FALSE(c.iot.actuatorOn());
    TEST_ASSERT_TRUE(logHas(logger, "old command ignored"));

    // A line larger than the buffer (attachment, title): dropped whole
    MockTLS::live->inject(std::string(NtfyNotifier::LINE_LEN + 50, 'x') + "\n");
    c.send(message("v2", "on"));
    c.ntfy.tick(11);                                 // reads up to 256 bytes per tick
    TEST_ASSERT_FALSE(c.iot.actuatorOn());
    c.ntfy.tick(12);
    c.ntfy.tick(13);
    TEST_ASSERT_TRUE(c.iot.actuatorOn());
}

void test_command_failures_wait_longer_each_time() {
    Cmd c;
    MockHTTP::rawHttp = "HTTP/1.1 401 Unauthorized\r\n\r\n";
    c.ntfy.tick(0);
    c.ntfy.tick(1);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: commands HTTP 401"));
    TEST_ASSERT_EQUAL_STRING("connecting", c.ntfy.commandStatus());

    MockHTTP::rawHttp = "junk\r\n\r\n";
    c.ntfy.tick(1 + 4999);                           // 5 s backoff
    TEST_ASSERT_FALSE(contains(MockHTTP::lastRaw, "junk"));
    int calls = MockTLS::connectCalls;
    c.ntfy.tick(1 + 5000);
    TEST_ASSERT_EQUAL(calls + 1, MockTLS::connectCalls);
    c.ntfy.tick(1 + 5001);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: commands HTTP 0"));

    MockTLS::failConnect = true;                     // then 10 s
    c.ntfy.tick(5002 + 9999);
    TEST_ASSERT_EQUAL(calls + 1, MockTLS::connectCalls);
    c.ntfy.tick(5002 + 10000);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: no connection for commands"));

    // The cap is 5 min
    unsigned long t = 5002 + 10000;
    for (int i = 0; i < 8; i++) {
        t += NtfyNotifier::BACKOFF_MAX_S * 1000UL;
        c.ntfy.tick(t);
    }
    calls = MockTLS::connectCalls;
    c.ntfy.tick(t + NtfyNotifier::BACKOFF_MAX_S * 1000UL);
    TEST_ASSERT_EQUAL(calls + 1, MockTLS::connectCalls);

    // Connecting resets the wait
    MockTLS::failConnect = false;
    t += NtfyNotifier::BACKOFF_MAX_S * 2000UL;
    c.open(t);
    TEST_ASSERT_EQUAL_STRING("connected", c.ntfy.commandStatus());
}

void test_commands_without_memory_tls_or_clock() {
    {
        Cmd c;
        ESP._heap = 1000;
        c.ntfy.tick(0);
        TEST_ASSERT_TRUE(logHas(logger, "not enough memory for commands"));
        TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    }
    resetMocks();
    {
        MockTLS::nullOwned = true;                   // TLS context without memory
        Cmd c;
        c.ntfy.tick(0);
        TEST_ASSERT_TRUE(logHas(logger, "no connection for commands"));
        TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    }
    resetMocks();
    {
        fakeNow = 1000;                               // clock still in 1970
        Cmd c;
        c.ntfy.tick(0);
        TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
        fakeNow = Clock::MIN_VALID_EPOCH + 100;
        MockHTTP::rawHttp = OK_HDR;
        c.ntfy.tick(NtfyNotifier::RECONNECT_MS - 1);
        TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
        c.ntfy.tick(NtfyNotifier::RECONNECT_MS);
        TEST_ASSERT_EQUAL(1, MockTLS::connectCalls);
    }
}

void test_commands_pause_and_turn_off() {
    Cmd c;
    c.open();
    TEST_ASSERT_TRUE(MockTLS::live->isConnected());

    // DNS or OTA will use the network: releases the session at once
    c.ntfy.hold(true);
    TEST_ASSERT_EQUAL_STRING("paused", c.ntfy.commandStatus());
    TEST_ASSERT_FALSE(MockTLS::live && MockTLS::live->isConnected());
    int calls = MockTLS::connectCalls;
    c.ntfy.tick(100);
    TEST_ASSERT_EQUAL(calls, MockTLS::connectCalls);
    c.ntfy.hold(false);
    c.open(200);                                      // comes back without waiting
    TEST_ASSERT_EQUAL(calls + 1, MockTLS::connectCalls);

    WiFi._status = WL_DISCONNECTED;
    c.ntfy.tick(300);
    TEST_ASSERT_FALSE(MockTLS::live && MockTLS::live->isConnected());
    WiFi._status = WL_CONNECTED;

    // No actuator or no topic: off
    cfg.ntfy_cmd[0] = '\0';
    c.ntfy.tick(400);
    TEST_ASSERT_EQUAL_STRING("off", c.ntfy.commandStatus());
    strlcpy(cfg.ntfy_cmd, "cmd-abc", sizeof(cfg.ntfy_cmd));
    cfg.iot.actuatorPin = -1;
    c.iot.begin();
    TEST_ASSERT_EQUAL_STRING("off", c.ntfy.commandStatus());
    NtfyNotifier noIot{cfg, logger, c.clock};
    TEST_ASSERT_EQUAL_STRING("off", noIot.commandStatus());
}

// An event goes out in seconds; a periodic reading, per minute
void test_events_go_out_fast_periodic_readings_do_not() {
    Fix f;
    strlcpy(cfg.iot.actuator, "relay", sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    iot.begin();
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    MockHTTP::push(200, "ok");
    ntfy.tick(0);                                     // IP at boot
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());

    iot.command("on", 100);
    TEST_ASSERT_TRUE(iot.urgent());
    ntfy.tick(NtfyNotifier::POST_GAP - 1);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    MockHTTP::push(200, "ok");
    ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[1].payload, "[actuator] on"));
    TEST_ASSERT_FALSE(iot.urgent());
}

// A periodic reading (no change) waits for the long interval
void test_periodic_reading_is_not_urgent() {
    Fix f;
    strlcpy(cfg.iot.sensor, "pulses", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::PULSE_WINDOW_MS);            // 1st reading: urgent
    TEST_ASSERT_TRUE(iot.urgent());
    MockHTTP::push(200, "ok");
    ntfy.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_FALSE(iot.pending());

    iot.tick(IoTService::PULSE_WINDOW_MS * 2);        // the same: periodic
    TEST_ASSERT_TRUE(iot.pending());
    TEST_ASSERT_FALSE(iot.urgent());
    ntfy.tick(IoTService::PULSE_WINDOW_MS * 2);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
}

// ── Action buttons on the notification ───────────────────────────────────────

static std::string header(const MockHTTP::Request& r, const char* name) {
    for (auto& h : r.headers)
        if (h.first == name) return h.second;
    return "";
}

// Publishes an actuator change and returns the Actions header
static std::string actionsFor(const char* actuator, const char* sensor = "") {
    resetMocks();
    logger = Logger();
    strlcpy(cfg.iot.actuator, actuator, sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;
    strlcpy(cfg.iot.sensor, sensor, sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = sensor[0] ? 2 : -1;
    cfg.iot.notify = true;
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    iot.begin();
    iot.command("on", 0);
    MockHTTP::push(200, "ok");
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(cloudflareTrust(MockHTTP::requests[0].trust));
    TEST_ASSERT_FALSE(MockHTTP::requests[0].insecure);
    return header(MockHTTP::requests[0], "Actions");
}

void test_actions_by_what_the_actuator_does() {
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));
    TEST_ASSERT_EQUAL_STRING(
        "http, On, https://ntfy.sh/cmd-x, method=POST, body=on, clear=true; "
        "http, Off, https://ntfy.sh/cmd-x, method=POST, body=off, clear=true; "
        "http, Pulse, https://ntfy.sh/cmd-x, method=POST, body=pulse 1, clear=true",
        actionsFor("relay").c_str());

    std::string level = actionsFor("fan");
    TEST_ASSERT_TRUE(contains(level, "http, Off,"));
    TEST_ASSERT_TRUE(contains(level, "http, 50%, https://ntfy.sh/cmd-x, method=POST, body=50%"));
    TEST_ASSERT_TRUE(contains(level, "http, 100%,"));

    std::string servo = actionsFor("servo");
    TEST_ASSERT_TRUE(contains(servo, "http, Open,"));
    TEST_ASSERT_TRUE(contains(servo, "body=close"));
    TEST_ASSERT_TRUE(contains(servo, "body=pulse 3"));

    TEST_ASSERT_TRUE(contains(actionsFor("strip"), "body=pulse 5"));

    // With the rule, the third button goes back to automatic
    std::string rule = actionsFor("relay", "pir");
    TEST_ASSERT_TRUE(contains(rule, "http, Auto, https://ntfy.sh/cmd-x, method=POST, body=auto"));
    TEST_ASSERT_FALSE(contains(rule, "Pulse"));
}

void test_actions_with_token_and_without_command_topic() {
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));
    strlcpy(cfg.ntfy_token, "tk_abc", sizeof(cfg.ntfy_token));
    std::string a = actionsFor("relay");
    TEST_ASSERT_TRUE(contains(a, "clear=true, headers.Authorization=Bearer tk_abc; http, Off"));

    cfg.ntfy_cmd[0] = '\0';
    TEST_ASSERT_EQUAL_STRING("", actionsFor("relay").c_str());
}

void test_actions_only_with_actuator() {
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));
    strlcpy(cfg.iot.sensor, "pir", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    cfg.iot.notify = true;
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    mock::pinIn[2] = HIGH;
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    MockHTTP::push(200, "ok");
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("", header(MockHTTP::requests[0], "Actions").c_str());

    // The IP and error replies never carry buttons
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "[sensor] motion"));
}

// ── Posting validates the certificate ────────────────────────────────────────

// IoT event: only the POST, with no IP lookup before
static void sensorEvent(IoTService& iot) {
    strlcpy(cfg.iot.sensor, "digital", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    cfg.iot.notify = true;
    mock::pinIn[2] = HIGH;
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(iot.pending());
}

// The post carries the token: the connection must validate the certificate
void test_post_validates_certificate() {
    strlcpy(cfg.ntfy_token, "tk_secret", sizeof(cfg.ntfy_token));
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    sensorEvent(iot);
    MockHTTP::push(200, "ok");
    ntfy.tick(0);

    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    const MockHTTP::Request& r = MockHTTP::requests[0];
    TEST_ASSERT_EQUAL_STRING("POST", r.method.c_str());
    TEST_ASSERT_EQUAL_STRING("Bearer tk_secret", header(r, "Authorization").c_str());
    TEST_ASSERT_TRUE(cloudflareTrust(r.trust));
    TEST_ASSERT_EQUAL((long)fakeNow, r.x509Time);
    TEST_ASSERT_FALSE(r.insecure);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
    TEST_ASSERT_EQUAL(5000UL, MockTLS::lastTimeoutMs);
    TEST_ASSERT_FALSE(iot.pending());
}

// Clock in 1970: BearSSL would reject the certificate. It does not even try;
// the event waits and goes out once the clock syncs.
void test_post_waits_for_clock() {
    fakeNow = 1000;
    strlcpy(cfg.ntfy_token, "tk_secret", sizeof(cfg.ntfy_token));
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    sensorEvent(iot);
    ntfy.tick(0);

    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    TEST_ASSERT_EQUAL(0, MockTLS::bufIn);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: clock not synchronized"));
    TEST_ASSERT_TRUE(iot.pending());

    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    ntfy.tick(NtfyNotifier::POST_GAP - 1);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    MockHTTP::push(200, "ok");
    ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(cloudflareTrust(MockHTTP::requests[0].trust));
    TEST_ASSERT_EQUAL((long)fakeNow, MockHTTP::requests[0].x509Time);
    TEST_ASSERT_FALSE(iot.pending());
}

// At the edge: MIN_VALID_EPOCH does not count as synced yet
void test_post_clock_at_the_edge() {
    fakeNow = Clock::MIN_VALID_EPOCH;
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    sensorEvent(iot);
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: clock not synchronized"));

    fakeNow = Clock::MIN_VALID_EPOCH + 1;
    MockHTTP::push(200, "ok");
    ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
}

// Certificate rejected (man in the middle, changed CA): the handshake fails,
// HTTPClient returns a negative code and the event waits for the next time
void test_post_rejected_certificate_retries() {
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    sensorEvent(iot);
    MockHTTP::push(-1);
    ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy error: -1"));
    TEST_ASSERT_TRUE(iot.pending());

    MockHTTP::push(200, "ok");
    ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_FALSE(iot.pending());
    for (auto& r : MockHTTP::requests) TEST_ASSERT_TRUE(cloudflareTrust(r.trust));
}

// Short on memory: neither the clock nor TLS is consulted
void test_post_without_memory_before_clock() {
    fakeNow = 1000;
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.clock, &iot};
    sensorEvent(iot);
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: not enough memory"));
    TEST_ASSERT_FALSE(logHas(logger, "ntfy: clock not synchronized"));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

// No ntfy connection, on any path, turns validation off
void test_no_path_uses_tls_without_validation() {
    strlcpy(cfg.ntfy_token, "tk_x", sizeof(cfg.ntfy_token));
    Cmd c;
    strlcpy(cfg.ntfy_topic, "data", sizeof(cfg.ntfy_topic));
    cfg.iot.notify = true;
    c.iot.begin();                                     // IoT copies the config in begin
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    MockHTTP::push(200, "ok");                        // IP at boot
    c.ntfy.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());

    c.open(1);                                         // command stream
    TEST_ASSERT_EQUAL_STRING("connected", c.ntfy.commandStatus());
    TEST_ASSERT_TRUE(cloudflareTrust(MockTLS::live->trust));
    TEST_ASSERT_FALSE(MockTLS::live->insecure);
    c.send(message("z1", "fly"));                    // error reply
    c.ntfy.tick(3);
    MockHTTP::push(200, "ok");
    c.ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[1].payload, "[error]"));

    c.iot.command("on", NtfyNotifier::POST_GAP);    // actuator change
    MockHTTP::push(200, "ok");
    c.ntfy.tick(NtfyNotifier::POST_GAP * 2);
    TEST_ASSERT_EQUAL(3, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[2].payload, "[actuator] on"));
    TEST_ASSERT_FALSE(header(MockHTTP::requests[2], "Actions").empty());

    for (auto& r : MockHTTP::requests) {
        TEST_ASSERT_TRUE(cloudflareTrust(r.trust));
        TEST_ASSERT_EQUAL((long)fakeNow, r.x509Time);
        TEST_ASSERT_FALSE(r.insecure);
    }
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_no_post_without_topic);
    RUN_TEST(test_without_ip_does_not_post);
    RUN_TEST(test_posts_at_boot_without_cloudflare);
    RUN_TEST(test_does_not_repost_same_ip);
    RUN_TEST(test_reposts_when_ip_changes);
    RUN_TEST(test_failure_waits_interval_to_retry);
    RUN_TEST(test_connection_error);
    RUN_TEST(test_negative_code);
    RUN_TEST(test_iot_without_reading_still_posts_ip);
    RUN_TEST(test_sensor_on_the_same_topic);
    RUN_TEST(test_not_enough_memory_does_not_post);
    RUN_TEST(test_tls_unavailable_does_not_post);
    RUN_TEST(test_validates_topic);
    RUN_TEST(test_commands_connect_validating_certificate);
    RUN_TEST(test_commands_reconnect_asking_for_missed);
    RUN_TEST(test_commands_with_token_and_error_reply);
    RUN_TEST(test_invalid_commands_reply_on_data_topic);
    RUN_TEST(test_commands_ignored_when_old_or_long);
    RUN_TEST(test_command_failures_wait_longer_each_time);
    RUN_TEST(test_commands_without_memory_tls_or_clock);
    RUN_TEST(test_commands_pause_and_turn_off);
    RUN_TEST(test_events_go_out_fast_periodic_readings_do_not);
    RUN_TEST(test_periodic_reading_is_not_urgent);
    RUN_TEST(test_actions_by_what_the_actuator_does);
    RUN_TEST(test_actions_with_token_and_without_command_topic);
    RUN_TEST(test_actions_only_with_actuator);
    RUN_TEST(test_post_validates_certificate);
    RUN_TEST(test_post_waits_for_clock);
    RUN_TEST(test_post_clock_at_the_edge);
    RUN_TEST(test_post_rejected_certificate_retries);
    RUN_TEST(test_post_without_memory_before_clock);
    RUN_TEST(test_no_path_uses_tls_without_validation);
    return UNITY_END();
}
