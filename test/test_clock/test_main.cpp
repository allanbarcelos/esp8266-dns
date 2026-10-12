// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Clock.h"

static time_t fakeNow = 0;
static time_t fakeTime() { return fakeNow; }

void setUp()    { resetMocks(); fakeNow = 0; }
void tearDown() {}

void test_begin_configura_sntp() {
    Clock clock(fakeTime);
    clock.begin();
    TEST_ASSERT_EQUAL(1, mock::configTimeCalls);
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", mock::ntpServer1.c_str());
}

void test_nao_sincronizado_em_1970() {
    Clock clock(fakeTime);
    fakeNow = 1000;
    TEST_ASSERT_FALSE(clock.synced());
    TEST_ASSERT_EQUAL(1000, (long)clock.now());
}

void test_sincronizado_apos_epoch_minima() {
    Clock clock(fakeTime);
    fakeNow = Clock::MIN_VALID_EPOCH + 1;
    TEST_ASSERT_TRUE(clock.synced());
}

void test_relogio_do_sistema_por_padrao() {
    Clock clock;
    TEST_ASSERT_TRUE(clock.now() > 0);
    TEST_ASSERT_TRUE(clock.synced()); // relógio do host está sincronizado
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_configura_sntp);
    RUN_TEST(test_nao_sincronizado_em_1970);
    RUN_TEST(test_sincronizado_apos_epoch_minima);
    RUN_TEST(test_relogio_do_sistema_por_padrao);
    return UNITY_END();
}
