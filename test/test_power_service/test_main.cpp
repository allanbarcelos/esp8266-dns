// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "PowerService.h"
#include "../i2c_sim.h"

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
}
void tearDown() {}

// Simulated MAX17048: 16-bit registers, most significant byte first
struct Gauge {
    I2cSim   sim{3, 1};
    uint16_t soc = 0, rate = 0, cell = 0;

    Gauge() {
        sim.present = {PowerService::ADDR};
        sim.reply = [this](uint8_t, const std::vector<uint8_t>& w) {
            uint16_t v = w.size() < 2 ? 0 : w[1] == 0x04 ? soc : w[1] == 0x16 ? rate : w[1] == 0x02 ? cell : 0xFFFF;
            return std::vector<uint8_t>{(uint8_t)(v >> 8), (uint8_t)v};
        };
    }
};

void test_without_gauge_is_on_supply() {
    PowerService p{cfg, logger};
    p.begin();
    p.tick(0);
    TEST_ASSERT_FALSE(p.configured());
    TEST_ASSERT_EQUAL(PowerService::MAINS, p.source());
    TEST_ASSERT_EQUAL(0, logger.count());
}

void test_gauge_not_answering() {
    cfg.bat_sda = 3;
    PowerService p{cfg, logger};
    p.begin();
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[3]);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[1]);
    TEST_ASSERT_EQUAL(PowerService::UNKNOWN, p.source());
    I2cSim sim(3, 1);                                     // nobody at the address
    p.tick(0);
    TEST_ASSERT_EQUAL(PowerService::UNKNOWN, p.source());
    TEST_ASSERT_TRUE(logHas(logger, "Power: gauge not answering"));
}

void test_battery_charging_and_supply() {
    cfg.bat_sda = 3;
    Gauge g;
    PowerService p{cfg, logger};
    p.begin();

    g.soc = 0x4C80;                                       // 76.5 %
    g.rate = (uint16_t)-25;                               // -5.2 %/h
    g.cell = 51200;                                       // 4.000 V
    p.tick(0);
    TEST_ASSERT_EQUAL(PowerService::BATTERY, p.source());
    TEST_ASSERT_EQUAL(76, p.percent());
    TEST_ASSERT_EQUAL(4000, p.millivolts());
    TEST_ASSERT_EQUAL(-52, p.rateTenths());
    TEST_ASSERT_TRUE(logHas(logger, "Power: on battery"));

    g.rate = 10;                                          // filling up
    p.tick(PowerService::READ_MS - 1);
    TEST_ASSERT_EQUAL(PowerService::BATTERY, p.source()); // has not read again yet
    p.tick(PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::CHARGING, p.source());
    TEST_ASSERT_TRUE(logHas(logger, "Power: charging"));

    g.rate = 0;                                           // full and idle
    g.soc = 0x6500;                                       // 101 %: stays at 100
    p.tick(2 * PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::MAINS, p.source());
    TEST_ASSERT_EQUAL(100, p.percent());
    TEST_ASSERT_TRUE(logHas(logger, "Power: on supply"));

    int lines = logger.count();
    g.soc = 0x6400;
    p.tick(3 * PowerService::READ_MS);                    // same state: no log
    TEST_ASSERT_EQUAL(lines, logger.count());

    g.soc = 0x3200;                                       // idle, but far from full
    p.tick(4 * PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::BATTERY, p.source());
    g.rate = (uint16_t)-3;
    g.soc = 0x6400;                                       // full, but draining
    p.tick(5 * PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::BATTERY, p.source());

    // Gone halfway: the next reading reports it
    g.sim.present.clear();
    p.tick(6 * PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::UNKNOWN, p.source());
}

// Any register can fail: the whole reading counts as no answer
void test_failing_register() {
    cfg.bat_sda = 3;
    Gauge g;
    g.soc = 0x6400;
    PowerService p{cfg, logger};
    p.begin();
    for (int nth : {1, 2}) {                              // refuses the 2nd or the 3rd register
        int reads = 0;
        g.sim.reply = [&](uint8_t, const std::vector<uint8_t>&) {
            reads++;
            return std::vector<uint8_t>{0x64, 0x00};
        };
        g.sim.present = {PowerService::ADDR};
        p.begin();
        // After nth reads, the gauge goes away
        auto base = g.sim.reply;
        g.sim.reply = [&, base, nth](uint8_t a, const std::vector<uint8_t>& w) {
            if (reads + 1 >= nth) g.sim.present.clear();
            return base(a, w);
        };
        p.tick(0);
        TEST_ASSERT_EQUAL(PowerService::UNKNOWN, p.source());
    }
}

void test_free_pins() {
    IoTConfig t;
    TEST_ASSERT_TRUE(PowerService::pinsFree(-1, t));
    TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
    t.actuatorPin = 1;                                    // actuator on SCL
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    t.actuatorPin = 3;                                    // on SDA
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    t.actuatorPin = 0;
    TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
    strlcpy(t.sensor, "pir", sizeof(t.sensor));
    t.sensorPin = 1;
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    t.sensorPin = 3;
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    strlcpy(t.sensor, "sht3x", sizeof(t.sensor));         // same bus
    TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
    t.sensorPin = 1;                                      // SHT3x with the roles swapped
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    t.sensorPin = 2;
    TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
    // Every I2C sensor shares the pair; an unknown type does not
    t.sensorPin = 3;
    for (const char* s : {"aht20", "bh1750"}) {
        strlcpy(t.sensor, s, sizeof(t.sensor));
        TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
    }
    strlcpy(t.sensor, "laser", sizeof(t.sensor));
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_without_gauge_is_on_supply);
    RUN_TEST(test_gauge_not_answering);
    RUN_TEST(test_battery_charging_and_supply);
    RUN_TEST(test_failing_register);
    RUN_TEST(test_free_pins);
    return UNITY_END();
}
