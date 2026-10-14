// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"

static Logger lgr;

void setUp()    { lgr = Logger(); resetMocks(); }
void tearDown() {}

void test_single_entry() {
    lgr.log("hello %s", "world");
    TEST_ASSERT_EQUAL(1, lgr.count());
    TEST_ASSERT_NOT_NULL(std::strstr(lgr.entry(0), "hello world"));
}

void test_timestamp_in_entry() {
    setMillis(5000);
    lgr.log("msg");
    TEST_ASSERT_NOT_NULL(std::strstr(lgr.entry(0), "[5s]"));
}

void test_buffer_does_not_exceed_limit() {
    for (int i = 0; i < LOG_BUFFER_SIZE + 3; i++) lgr.log("msg %d", i);
    TEST_ASSERT_EQUAL(LOG_BUFFER_SIZE, lgr.count());
}

void test_ring_buffer_order() {
    for (int i = 0; i < LOG_BUFFER_SIZE + 2; i++) lgr.log("item %d", i);

    int start = lgr.startIndex();
    TEST_ASSERT_NOT_NULL(std::strstr(lgr.entry(start), "item 2"));

    int last = (start + lgr.count() - 1) % LOG_BUFFER_SIZE;
    char expected[32];
    std::snprintf(expected, sizeof(expected), "item %d", LOG_BUFFER_SIZE + 1);
    TEST_ASSERT_NOT_NULL(std::strstr(lgr.entry(last), expected));
}

void test_count_before_wrap() {
    lgr.log("a"); lgr.log("b"); lgr.log("c");
    TEST_ASSERT_EQUAL(3, lgr.count());
    TEST_ASSERT_EQUAL(0, lgr.startIndex());
}

void test_count_after_wrap() {
    for (int i = 0; i < LOG_BUFFER_SIZE; i++) lgr.log("x");
    TEST_ASSERT_EQUAL(LOG_BUFFER_SIZE, lgr.count());

    lgr.log("extra");
    TEST_ASSERT_EQUAL(LOG_BUFFER_SIZE, lgr.count());
    TEST_ASSERT_EQUAL(1, lgr.startIndex());
}

void test_long_message_is_truncated() {
    std::string big(500, 'A');
    lgr.log("%s", big.c_str());
    TEST_ASSERT_TRUE(std::strlen(lgr.entry(0)) < LOG_LINE_SIZE);
}

void test_entry_ring_index() {
    lgr.log("zero");
    TEST_ASSERT_EQUAL_STRING(lgr.entry(0), lgr.entry(LOG_BUFFER_SIZE));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_single_entry);
    RUN_TEST(test_timestamp_in_entry);
    RUN_TEST(test_buffer_does_not_exceed_limit);
    RUN_TEST(test_ring_buffer_order);
    RUN_TEST(test_count_before_wrap);
    RUN_TEST(test_count_after_wrap);
    RUN_TEST(test_long_message_is_truncated);
    RUN_TEST(test_entry_ring_index);
    return UNITY_END();
}
