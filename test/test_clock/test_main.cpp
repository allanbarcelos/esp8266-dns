// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Clock.h"

static time_t fakeNow = 0;
static time_t fakeTime() { return fakeNow; }

void setUp()    { resetMocks(); fakeNow = 0; }
void tearDown() {}

void test_begin_configures_sntp() {
    Clock clock(fakeTime);
    clock.begin();
    TEST_ASSERT_EQUAL(1, mock::configTimeCalls);
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", mock::ntpServer1.c_str());
}

// One SNTP server at a time (lwIP without extras): alternates until synced
void test_without_time_alternates_server() {
    Clock clock(fakeTime);
    clock.begin();
    clock.tick(Clock::RETRY_MS - 1);
    TEST_ASSERT_EQUAL(1, mock::configTimeCalls);
    clock.tick(Clock::RETRY_MS);
    TEST_ASSERT_EQUAL(2, mock::configTimeCalls);
    TEST_ASSERT_EQUAL_STRING("time.google.com", mock::ntpServer1.c_str());
    clock.tick(2 * Clock::RETRY_MS);
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", mock::ntpServer1.c_str());

    fakeNow = Clock::MIN_VALID_EPOCH + 1;
    clock.tick(10 * Clock::RETRY_MS);
    TEST_ASSERT_EQUAL(3, mock::configTimeCalls);   // synced: stays where it is
}

void test_not_synced_in_1970() {
    Clock clock(fakeTime);
    fakeNow = 1000;
    TEST_ASSERT_FALSE(clock.synced());
    TEST_ASSERT_EQUAL(1000, (long)clock.now());
}

void test_synced_after_min_epoch() {
    Clock clock(fakeTime);
    fakeNow = Clock::MIN_VALID_EPOCH + 1;
    TEST_ASSERT_TRUE(clock.synced());
}

void test_system_clock_by_default() {
    Clock clock;
    TEST_ASSERT_TRUE(clock.now() > 0);
    TEST_ASSERT_TRUE(clock.synced()); // the host clock is synced
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_configures_sntp);
    RUN_TEST(test_without_time_alternates_server);
    RUN_TEST(test_not_synced_in_1970);
    RUN_TEST(test_synced_after_min_epoch);
    RUN_TEST(test_system_clock_by_default);
    return UNITY_END();
}
