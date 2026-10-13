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

// MAX17048 simulado: registros de 16 bits, o mais significativo primeiro
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

void test_sem_medidor_esta_na_fonte() {
    PowerService p{cfg, logger};
    p.begin();
    p.tick(0);
    TEST_ASSERT_FALSE(p.configured());
    TEST_ASSERT_EQUAL(PowerService::MAINS, p.source());
    TEST_ASSERT_EQUAL(0, logger.count());
}

void test_medidor_sem_resposta() {
    cfg.bat_sda = 3;
    PowerService p{cfg, logger};
    p.begin();
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[3]);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[1]);
    TEST_ASSERT_EQUAL(PowerService::UNKNOWN, p.source());
    I2cSim sim(3, 1);                                     // ninguém no endereço
    p.tick(0);
    TEST_ASSERT_EQUAL(PowerService::UNKNOWN, p.source());
    TEST_ASSERT_TRUE(logHas(logger, "Energia: medidor sem resposta"));
}

void test_bateria_carregando_e_fonte() {
    cfg.bat_sda = 3;
    Gauge g;
    PowerService p{cfg, logger};
    p.begin();

    g.soc = 0x4C80;                                       // 76,5 %
    g.rate = (uint16_t)-25;                               // -5,2 %/h
    g.cell = 51200;                                       // 4,000 V
    p.tick(0);
    TEST_ASSERT_EQUAL(PowerService::BATTERY, p.source());
    TEST_ASSERT_EQUAL(76, p.percent());
    TEST_ASSERT_EQUAL(4000, p.millivolts());
    TEST_ASSERT_EQUAL(-52, p.rateTenths());
    TEST_ASSERT_TRUE(logHas(logger, "Energia: na bateria"));

    g.rate = 10;                                          // enchendo
    p.tick(PowerService::READ_MS - 1);
    TEST_ASSERT_EQUAL(PowerService::BATTERY, p.source()); // ainda não leu de novo
    p.tick(PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::CHARGING, p.source());
    TEST_ASSERT_TRUE(logHas(logger, "Energia: carregando"));

    g.rate = 0;                                           // cheia e parada
    g.soc = 0x6500;                                       // 101 %: fica em 100
    p.tick(2 * PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::MAINS, p.source());
    TEST_ASSERT_EQUAL(100, p.percent());
    TEST_ASSERT_TRUE(logHas(logger, "Energia: na fonte"));

    int lines = logger.count();
    g.soc = 0x6400;
    p.tick(3 * PowerService::READ_MS);                    // mesma situação: sem log
    TEST_ASSERT_EQUAL(lines, logger.count());

    g.soc = 0x3200;                                       // parada, mas longe de cheia
    p.tick(4 * PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::BATTERY, p.source());
    g.rate = (uint16_t)-3;
    g.soc = 0x6400;                                       // cheia, mas gastando
    p.tick(5 * PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::BATTERY, p.source());

    // Some no meio: a leitura seguinte avisa
    g.sim.present.clear();
    p.tick(6 * PowerService::READ_MS);
    TEST_ASSERT_EQUAL(PowerService::UNKNOWN, p.source());
}

// Cada registro pode falhar: a leitura toda vale como sem resposta
void test_registro_que_falha() {
    cfg.bat_sda = 3;
    Gauge g;
    g.soc = 0x6400;
    PowerService p{cfg, logger};
    p.begin();
    for (int nth : {1, 2}) {                              // recusa o 2º ou o 3º registro
        int reads = 0;
        g.sim.reply = [&](uint8_t, const std::vector<uint8_t>&) {
            reads++;
            return std::vector<uint8_t>{0x64, 0x00};
        };
        g.sim.present = {PowerService::ADDR};
        p.begin();
        // Depois de nth leituras, o medidor some
        auto base = g.sim.reply;
        g.sim.reply = [&, base, nth](uint8_t a, const std::vector<uint8_t>& w) {
            if (reads + 1 >= nth) g.sim.present.clear();
            return base(a, w);
        };
        p.tick(0);
        TEST_ASSERT_EQUAL(PowerService::UNKNOWN, p.source());
    }
}

void test_pinos_livres() {
    IoTConfig t;
    TEST_ASSERT_TRUE(PowerService::pinsFree(-1, t));
    TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
    t.actuatorPin = 1;                                    // atuador no SCL
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    t.actuatorPin = 3;                                    // no SDA
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    t.actuatorPin = 0;
    TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
    strlcpy(t.sensor, "pir", sizeof(t.sensor));
    t.sensorPin = 1;
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    t.sensorPin = 3;
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    strlcpy(t.sensor, "sht3x", sizeof(t.sensor));         // mesmo barramento
    TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
    t.sensorPin = 1;                                      // SHT3x com os papéis trocados
    TEST_ASSERT_FALSE(PowerService::pinsFree(3, t));
    t.sensorPin = 2;
    TEST_ASSERT_TRUE(PowerService::pinsFree(3, t));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sem_medidor_esta_na_fonte);
    RUN_TEST(test_medidor_sem_resposta);
    RUN_TEST(test_bateria_carregando_e_fonte);
    RUN_TEST(test_registro_que_falha);
    RUN_TEST(test_pinos_livres);
    return UNITY_END();
}
