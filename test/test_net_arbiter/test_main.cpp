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

void test_names() {
    const char* want[] = {"ota", "ip", "dns", "cloudflare", "ntfy post", "relay", "ntfy stream", "-"};
    for (int i = 0; i <= NetArbiter::COUNT; i++)
        TEST_ASSERT_EQUAL_STRING(want[i], NetArbiter::name((NetArbiter::Who)i));
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_turn_by_priority);
    RUN_TEST(test_turn_waits_for_the_heap);
    RUN_TEST(test_running_session_goes_on);
    RUN_TEST(test_new_owner_clears_the_wait);
    RUN_TEST(test_names);
    return UNITY_END();
}
