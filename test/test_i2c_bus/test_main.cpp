// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "I2cBus.h"
#include "../i2c_sim.h"

void setUp()    { resetMocks(); }
void tearDown() {}

using Bytes = std::vector<uint8_t>;

void test_pares_de_pinos() {
    TEST_ASSERT_EQUAL(2, I2cBus::sclFor(0));
    TEST_ASSERT_EQUAL(0, I2cBus::sclFor(2));
    TEST_ASSERT_EQUAL(1, I2cBus::sclFor(3));
    TEST_ASSERT_EQUAL(3, I2cBus::sclFor(1));
}

void test_begin_solta_as_linhas() {
    I2cBus bus(0, 2);
    bus.begin();
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[0]);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
}

void test_escrita_com_e_sem_aparelho() {
    I2cSim sim(0, 2);
    sim.present = {0x44};
    I2cBus bus(0, 2);
    const uint8_t cmd[] = {0x24, 0x00, 0xA5};
    TEST_ASSERT_TRUE(bus.write(0x44, cmd, 3));
    TEST_ASSERT_EQUAL(1, (int)sim.writes.size());
    TEST_ASSERT_TRUE(sim.writes[0] == Bytes({0x44, 0x24, 0x00, 0xA5}));
    TEST_ASSERT_EQUAL(1, sim.starts);
    TEST_ASSERT_EQUAL(1, sim.stops);

    TEST_ASSERT_FALSE(bus.write(0x45, cmd, 3));           // ninguém responde
    TEST_ASSERT_EQUAL(1, (int)sim.writes.size());
    TEST_ASSERT_EQUAL(2, sim.stops);

    sim.nackAfter = 1;                                    // recusa o 2º byte
    TEST_ASSERT_FALSE(bus.write(0x44, cmd, 3));
    TEST_ASSERT_TRUE(sim.writes.back() == Bytes({0x44, 0x24}));
    TEST_ASSERT_EQUAL(3, sim.stops);
}

void test_leitura_e_registro_de_16_bits() {
    I2cSim sim(3, 1);
    sim.present = {0x36};
    sim.reply = [](uint8_t addr, const Bytes& w) {
        TEST_ASSERT_EQUAL_HEX8(0x36, addr);
        return w.size() > 1 && w[1] == 0x04 ? Bytes{0x5A, 0x80} : Bytes{0xDE, 0xAD, 0xBE};
    };
    I2cBus bus(3, 1);
    uint8_t d[3] = {};
    TEST_ASSERT_TRUE(bus.read(0x36, d, 3));
    TEST_ASSERT_EQUAL_HEX8(0xDE, d[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, d[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, d[2]);

    uint16_t v = 0;
    int stops = sim.stops;
    TEST_ASSERT_TRUE(bus.readReg16(0x36, 0x04, v));
    TEST_ASSERT_EQUAL_HEX16(0x5A80, v);
    TEST_ASSERT_EQUAL(stops + 1, sim.stops);              // start repetido, um stop só

    // Mais bytes do que o aparelho tem: o resto vem em 0xFF
    sim.reply = [](uint8_t, const Bytes&) { return Bytes{0x01}; };
    TEST_ASSERT_TRUE(bus.read(0x36, d, 2));
    TEST_ASSERT_EQUAL_HEX8(0x01, d[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, d[1]);

    TEST_ASSERT_FALSE(bus.read(0x37, d, 2));
    TEST_ASSERT_FALSE(bus.readReg16(0x37, 0x04, v));
    sim.refuseRead = true;                                // aceita o registro, recusa a leitura
    TEST_ASSERT_FALSE(bus.readReg16(0x36, 0x04, v));
    sim.refuseRead = false;
    sim.nackAfter = 0;                                    // recusa o registro
    TEST_ASSERT_FALSE(bus.readReg16(0x36, 0x04, v));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pares_de_pinos);
    RUN_TEST(test_begin_solta_as_linhas);
    RUN_TEST(test_escrita_com_e_sem_aparelho);
    RUN_TEST(test_leitura_e_registro_de_16_bits);
    return UNITY_END();
}
