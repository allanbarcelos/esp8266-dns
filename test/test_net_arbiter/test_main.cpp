// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "NetArbiter.h"

void setUp()    { resetMocks(); }
void tearDown() {}

static const NetArbiter::Need SMALL = {18000, 8192};
static const NetArbiter::Need BIG   = {24000, 19000};

// The most important want gets the turn; the wants start over each tick
void test_turn_by_priority() {
    NetArbiter n;
    TEST_ASSERT_EQUAL(NetArbiter::NONE, n.grant());
    n.want(NetArbiter::STREAM);
    n.want(NetArbiter::DNS);
    n.want(NetArbiter::RELAY);
    TEST_ASSERT_EQUAL(NetArbiter::DNS, n.grant());
    TEST_ASSERT_EQUAL(NetArbiter::DNS, n.owner());
    char q[NetArbiter::QUEUE_LEN];
    TEST_ASSERT_EQUAL_STRING("dns, waiting: relay, ntfy stream", n.queue(q));

    n.want(NetArbiter::RELAY);
    n.want(NetArbiter::OTA);
    TEST_ASSERT_EQUAL(NetArbiter::OTA, n.grant());
    TEST_ASSERT_EQUAL_STRING("ota, waiting: relay", n.queue(q));
    TEST_ASSERT_EQUAL(NetArbiter::NONE, n.grant());
    TEST_ASSERT_EQUAL_STRING("-", n.queue(q));
}

// A new session waits for the heap it needs, up to WAIT_MS; then it goes anyway
void test_turn_waits_for_the_heap() {
    NetArbiter n;
    n.want(NetArbiter::OTA);
    n.grant();
    ESP._heap = 30000;
    ESP._maxBlock = 25000;
    TEST_ASSERT_TRUE(n.ready(0, BIG));
    TEST_ASSERT_FALSE(n.starved());

    ESP._heap = 22000;                                    // short of heap
    TEST_ASSERT_FALSE(n.ready(1000, BIG));
    TEST_ASSERT_TRUE(n.starved());
    char q[NetArbiter::QUEUE_LEN];
    TEST_ASSERT_EQUAL_STRING("ota (memory)", n.queue(q));
    TEST_ASSERT_FALSE(n.ready(1000 + NetArbiter::WAIT_MS - 1, BIG));
    TEST_ASSERT_TRUE(n.ready(1000 + NetArbiter::WAIT_MS, BIG));   // waited enough
    TEST_ASSERT_EQUAL(1, n.late());
    TEST_ASSERT_FALSE(n.starved());
    TEST_ASSERT_FALSE(n.ready(1000 + NetArbiter::WAIT_MS + 1, BIG));   // a new wait

    ESP._heap = 30000;
    ESP._maxBlock = 10000;                                // short of one block
    TEST_ASSERT_FALSE(n.ready(50000, BIG));
    TEST_ASSERT_TRUE(n.ready(50000, SMALL));
    TEST_ASSERT_FALSE(n.starved());
}

// An open session already holds its heap: it goes on
void test_running_session_goes_on() {
    NetArbiter n;
    n.want(NetArbiter::RELAY);
    n.grant();
    ESP._heap = 9000;
    TEST_ASSERT_TRUE(n.ready(0, SMALL, true));
    TEST_ASSERT_FALSE(n.starved());
}

// Another owner starts its own wait
void test_new_owner_clears_the_wait() {
    NetArbiter n;
    ESP._heap = 9000;
    n.want(NetArbiter::IP);
    n.grant();
    TEST_ASSERT_FALSE(n.ready(0, SMALL));
    TEST_ASSERT_TRUE(n.starved());
    n.want(NetArbiter::IP);
    n.grant();                                            // same owner: still waiting
    TEST_ASSERT_TRUE(n.starved());
    n.want(NetArbiter::DNS);
    n.grant();
    TEST_ASSERT_FALSE(n.starved());
}

void test_slowest_loop() {
    NetArbiter n;
    n.loop(NetArbiter::SLOW_MS - 1);
    TEST_ASSERT_EQUAL(0, n.slow());
    TEST_ASSERT_EQUAL(0UL, n.worst());
    n.want(NetArbiter::RELAY);
    n.grant();
    n.loop(1500);
    n.want(NetArbiter::IP);
    n.grant();
    n.loop(900);
    TEST_ASSERT_EQUAL(2, n.slow());
    TEST_ASSERT_EQUAL(1500UL, n.worst());
    TEST_ASSERT_EQUAL(NetArbiter::RELAY, n.worstWho());
    TEST_ASSERT_EQUAL_STRING("relay", NetArbiter::name(n.worstWho()));
    for (int i = 0; i < 70000; i++) n.loop(NetArbiter::SLOW_MS);
    TEST_ASSERT_EQUAL(0xFFFF, n.slow());                   // the count stops, it does not wrap
}

void test_names() {
    const char* want[] = {"ota", "ip", "dns", "cloudflare", "ntfy post", "relay", "ntfy stream", "-",
                          "web", "wifi", "iot", "mqtt"};
    for (int i = 0; i <= NetArbiter::MQTT; i++)
        TEST_ASSERT_EQUAL_STRING(want[i], NetArbiter::name((uint8_t)i));
    // The longest queue fits
    NetArbiter n;
    ESP._heap = 0;
    for (int i = NetArbiter::SETUP; i < NetArbiter::COUNT; i++) n.want((NetArbiter::Who)i);
    n.want(NetArbiter::OTA);
    n.want(NetArbiter::IP);
    n.want(NetArbiter::DNS);
    n.grant();
    n.ready(0, SMALL);
    char q[NetArbiter::QUEUE_LEN];
    TEST_ASSERT_EQUAL_STRING("ota (memory), waiting: ip, dns, cloudflare, ntfy post, relay, ntfy stream", n.queue(q));
}

// A crash leaves where the loop was, and the first code address on the
// stack, for the next boot; once
void test_crash_is_kept_for_the_next_boot() {
    char out[NetArbiter::CRASH_LEN];
    NetArbiter n;
    TEST_ASSERT_FALSE(n.lastCrash(out));                  // a clean boot

    n.at(NetArbiter::IP);
    uint32_t stack[70] = {0x3FFF0000u, 0x40100000u, 0x40300000u, 0x40212345u, 0x40234567u};
    n.crashed(stack, stack + 70);
    NetArbiter next;
    TEST_ASSERT_TRUE(next.lastCrash(out));
    TEST_ASSERT_EQUAL_STRING("ip near 0x40212345", out);
    TEST_ASSERT_FALSE(next.lastCrash(out));               // read once

    n.at(NetArbiter::MQTT);                               // nothing on the stack (64 words looked at)
    for (auto& w : stack) w = 0;
    stack[64] = 0x40212345u;
    n.crashed(stack, stack + 70);
    TEST_ASSERT_TRUE(next.lastCrash(out));
    TEST_ASSERT_EQUAL_STRING("mqtt near 0x00000000", out);

    n.crashed(stack, stack);                              // an empty stack
    ESP._rtc[121] = NetArbiter::MQTT + 1;                 // a slot that makes no sense
    TEST_ASSERT_FALSE(next.lastCrash(out));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_turn_by_priority);
    RUN_TEST(test_turn_waits_for_the_heap);
    RUN_TEST(test_running_session_goes_on);
    RUN_TEST(test_new_owner_clears_the_wait);
    RUN_TEST(test_slowest_loop);
    RUN_TEST(test_names);
    RUN_TEST(test_crash_is_kept_for_the_next_boot);
    return UNITY_END();
}
