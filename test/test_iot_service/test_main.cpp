#include <unity.h>
#include "../test_support.h"
#include "IoTService.h"

static Logger logger;
static Config cfg;

void setUp()    { resetMocks(); logger = Logger(); cfg = Config{}; }
void tearDown() {}

static void arm(int sensor, int module) {
    cfg.iot_sensor_pin = (int8_t)sensor;
    cfg.iot_module_pin = (int8_t)module;
}

void test_pinos_e_parse() {
    TEST_ASSERT_TRUE(IoTService::allowedPin(0));
    TEST_ASSERT_TRUE(IoTService::allowedPin(1));
    TEST_ASSERT_TRUE(IoTService::allowedPin(2));
    TEST_ASSERT_TRUE(IoTService::allowedPin(3));
    TEST_ASSERT_FALSE(IoTService::allowedPin(-1));
    TEST_ASSERT_FALSE(IoTService::allowedPin(4));

    int8_t pin = 9;
    TEST_ASSERT_TRUE(IoTService::parsePin("-1", pin));
    TEST_ASSERT_EQUAL(-1, pin);
    TEST_ASSERT_TRUE(IoTService::parsePin("0", pin));
    TEST_ASSERT_EQUAL(0, pin);
    TEST_ASSERT_TRUE(IoTService::parsePin("3", pin));
    TEST_ASSERT_EQUAL(3, pin);
    TEST_ASSERT_FALSE(IoTService::parsePin("", pin));
    TEST_ASSERT_FALSE(IoTService::parsePin("12", pin));
    TEST_ASSERT_FALSE(IoTService::parsePin("9", pin));
    TEST_ASSERT_FALSE(IoTService::parsePin(" ", pin));
    TEST_ASSERT_FALSE(IoTService::parsePin("a", pin));
}

void test_desligado_nao_mexe_nos_pinos() {
    IoTService iot{cfg, logger};
    iot.begin();
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.moduleText());
    TEST_ASSERT_EQUAL(255, mock::pinModeV[2]);
    TEST_ASSERT_FALSE(iot.pending());
}

void test_so_sensor_notifica_depois_do_debounce() {
    arm(2, -1);
    cfg.iot_notify = true;
    IoTService iot{cfg, logger};
    mock::pinIn[2] = HIGH;
    iot.begin();
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
    TEST_ASSERT_TRUE(logHas(logger, "IoT: sensor GPIO2"));
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());

    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
    iot.tick(IoTService::DEBOUNCE_MS - 1);
    TEST_ASSERT_FALSE(iot.pending());

    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL_STRING("alto", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.moduleText());
    TEST_ASSERT_EQUAL_STRING("[sensor] GPIO2 alto", iot.message());
    iot.ack();
    TEST_ASSERT_FALSE(iot.pending());

    iot.tick(IoTService::DEBOUNCE_MS + 10);
    TEST_ASSERT_FALSE(iot.pending());

    mock::pinIn[2] = LOW;
    iot.tick(1000);
    iot.tick(1000 + IoTService::DEBOUNCE_MS - 1);
    TEST_ASSERT_EQUAL_STRING("alto", iot.sensorText());
    mock::pinIn[2] = HIGH;
    iot.tick(1000 + IoTService::DEBOUNCE_MS);
    iot.tick(2000);
    TEST_ASSERT_EQUAL_STRING("alto", iot.sensorText());
    TEST_ASSERT_FALSE(iot.pending());
}

void test_rele_acompanha_o_sensor() {
    arm(2, 0);
    cfg.iot_sensor_high = true;
    cfg.iot_module_high = false;
    cfg.iot_notify = true;
    IoTService iot{cfg, logger};
    mock::pinIn[2] = HIGH;
    iot.begin();
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[0]);
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);
    TEST_ASSERT_EQUAL_STRING("desligado", iot.moduleText());

    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(iot.moduleOn());
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[0]);
    TEST_ASSERT_EQUAL_STRING("ligado", iot.moduleText());
    TEST_ASSERT_TRUE(contains(std::string(iot.message()), "modulo ligado"));

    mock::pinIn[2] = LOW;
    iot.tick(1000);
    iot.tick(1000 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_FALSE(iot.moduleOn());
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);
    TEST_ASSERT_TRUE(contains(std::string(iot.message()), "modulo desligado"));
    TEST_ASSERT_EQUAL_STRING("baixo", iot.sensorText());

    cfg.iot_sensor_pin = -1;
    iot.begin();
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[0]);
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);
    TEST_ASSERT_FALSE(iot.moduleOn());
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.sensorText());
}

void test_niveis_invertidos_e_sem_ntfy() {
    arm(1, 3);
    cfg.iot_sensor_high = false;
    cfg.iot_module_high = true;
    cfg.iot_notify = false;
    IoTService iot{cfg, logger};
    mock::pinIn[1] = LOW;
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(iot.moduleOn());
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[3]);
    TEST_ASSERT_FALSE(iot.pending());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: GPIO1 baixo"));

    mock::pinIn[1] = HIGH;
    iot.tick(1000);
    iot.tick(1000 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_FALSE(iot.moduleOn());
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[3]);
    TEST_ASSERT_TRUE(logHas(logger, "IoT: GPIO1 alto"));

    cfg.iot_sensor_pin = -1;
    iot.begin();
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[3]);
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[3]);
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.moduleText());
}

void test_mesmo_pino_nao_vira_saida() {
    arm(2, 2);
    cfg.iot_notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.moduleText());
    mock::pinIn[2] = HIGH;
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_FALSE(iot.moduleOn());
    TEST_ASSERT_EQUAL_STRING("[sensor] GPIO2 alto", iot.message());
}

static void seg(int level, unsigned long dur) {
    mock::busSegs.push_back(mock::BusSeg{level, dur});
}

static void feedDht(uint8_t pin, const uint8_t d[5]) {
    mock::bus = true;
    mock::busPin = pin;
    mock::busAt = 0;
    mock::busSegs.clear();
    seg(LOW, 80);
    seg(HIGH, 80);
    for (int i = 0; i < 5; i++) {
        for (int b = 7; b >= 0; b--) {
            int bit = (d[i] >> b) & 1;
            seg(LOW, 50);
            seg(HIGH, bit ? 80 : 20);
        }
    }
    seg(LOW, 100);
}

static void frame(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t out[5]) {
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
    out[4] = (uint8_t)(a + b + c + d);
}

static void pushByte(uint8_t v) {
    for (int i = 0; i < 8; i++) seg(((v >> i) & 1) ? HIGH : LOW, 2);
}

static void feedDs(uint8_t pin, bool startOk, bool readOk, const uint8_t* bytes, int n) {
    mock::bus = true;
    mock::busPin = pin;
    mock::busAt = 0;
    mock::busSegs.clear();
    seg(startOk ? LOW : HIGH, 2);
    seg(readOk ? LOW : HIGH, 2);
    for (int i = 0; i < n; i++) pushByte(bytes[i]);
}

void test_parse_faixa() {
    int16_t v = 7;
    uint8_t u = 7;
    TEST_ASSERT_TRUE(IoTService::parseI16("0", -55, 100, v));
    TEST_ASSERT_EQUAL(0, v);
    TEST_ASSERT_TRUE(IoTService::parseI16("-55", -55, 100, v));
    TEST_ASSERT_EQUAL(-55, v);
    TEST_ASSERT_TRUE(IoTService::parseI16("100", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("1234567", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("-", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16(" ", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("1a", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("-60", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("101", -55, 100, v));
    TEST_ASSERT_TRUE(IoTService::parseU8("4", 0, 4, u));
    TEST_ASSERT_EQUAL(4, u);
    TEST_ASSERT_FALSE(IoTService::parseU8("9", 0, 4, u));
}

void test_tipo_desconhecido_vira_digital() {
    arm(2, 0);
    cfg.iot_sensor_type = 9;
    cfg.iot_module_type = 9;
    IoTService iot{cfg, logger};
    mock::pinIn[2] = HIGH;
    iot.begin();
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[0]);
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL_STRING("alto", iot.sensorText());
}

void test_servo_acompanha_o_sensor() {
    arm(2, 0);
    cfg.iot_module_type = IoTService::MODULE_SERVO;
    cfg.iot_angle_on = 180;
    cfg.iot_angle_off = 20;
    cfg.iot_sensor_high = true;
    cfg.iot_notify = true;
    IoTService iot{cfg, logger};
    mock::pinIn[2] = HIGH;
    iot.begin();
    TEST_ASSERT_TRUE(mock::servoAttached);
    TEST_ASSERT_EQUAL(0, mock::servoPin);
    TEST_ASSERT_EQUAL(20, mock::servoAngle);
    TEST_ASSERT_EQUAL_STRING("servo 20", iot.moduleText());

    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL(180, mock::servoAngle);
    TEST_ASSERT_EQUAL_STRING("servo 180", iot.moduleText());
    TEST_ASSERT_EQUAL_STRING("[sensor] GPIO2 alto servo 180", iot.message());

    mock::pinIn[2] = LOW;
    iot.tick(1000);
    iot.tick(1000 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL(20, mock::servoAngle);

    cfg.iot_sensor_pin = -1;
    iot.begin();
    TEST_ASSERT_FALSE(mock::servoAttached);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[0]);
}

void test_servo_nos_outros_pinos_e_falha_ao_anexar() {
    arm(0, 2);
    cfg.iot_module_type = IoTService::MODULE_SERVO;
    IoTService a{cfg, logger};
    a.begin();
    cfg.iot_sensor_pin = -1;
    a.begin();
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);

    arm(2, 1);
    cfg.iot_sensor_pin = 2;
    cfg.iot_module_type = IoTService::MODULE_SERVO;
    IoTService b{cfg, logger};
    b.begin();
    cfg.iot_sensor_pin = -1;
    b.begin();
    TEST_ASSERT_EQUAL(255, mock::pinModeV[1]);
    TEST_ASSERT_FALSE(mock::servoAttached);

    mock::servoAttachResult = 0;
    mock::servoWrites = 0;
    arm(2, 0);
    cfg.iot_sensor_pin = 2;
    cfg.iot_notify = true;
    IoTService c{cfg, logger};
    mock::pinIn[2] = HIGH;
    c.begin();
    TEST_ASSERT_TRUE(logHas(logger, "IoT: servo indisponivel"));
    TEST_ASSERT_EQUAL_STRING("falha", c.moduleText());
    c.tick(0);
    c.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL(0, mock::servoWrites);
    TEST_ASSERT_TRUE(contains(std::string(c.message()), "servo falha"));
    cfg.iot_sensor_pin = -1;
    c.begin();
    TEST_ASSERT_FALSE(mock::servoAttached);
}

void test_dht22_umidade_modulo_e_margem() {
    arm(2, 0);
    cfg.iot_sensor_type = IoTService::SENSOR_DHT22;
    cfg.iot_above = true;
    cfg.iot_threshold = 40;
    cfg.iot_margin = 2;
    cfg.iot_notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());

    uint8_t d[5];
    frame(0x01, 0xE7, 0x01, 0x09, d); // 48.7% e 26.5C
    feedDht(2, d);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("umid 48.7% temp 26.5C", iot.sensorText());
    TEST_ASSERT_TRUE(iot.moduleOn());
    TEST_ASSERT_EQUAL_STRING("[sensor] GPIO2 umid 48.7% temp 26.5C modulo ligado", iot.message());
    uint8_t logged = logger.count();

    iot.tick(IoTService::READ_GAP_MS - 1);
    TEST_ASSERT_EQUAL(logged, logger.count());

    feedDht(2, d);
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL(logged, logger.count());
    TEST_ASSERT_TRUE(iot.pending());

    frame(0x01, 0x81, 0x01, 0x09, d); // 38.5%
    feedDht(2, d);
    iot.tick(IoTService::READ_GAP_MS * 2);
    TEST_ASSERT_TRUE(iot.moduleOn());

    frame(0x01, 0x72, 0x01, 0x09, d); // 37.0%
    feedDht(2, d);
    iot.tick(IoTService::READ_GAP_MS * 3);
    TEST_ASSERT_FALSE(iot.moduleOn());
    TEST_ASSERT_TRUE(contains(std::string(iot.message()), "modulo desligado"));
}

void test_dht_falhas_temperatura_e_dht11() {
    arm(2, -1);
    cfg.iot_sensor_type = IoTService::SENSOR_DHT22;
    cfg.iot_metric = 1;
    cfg.iot_above = false;
    cfg.iot_threshold = 0;
    cfg.iot_notify = true;
    IoTService iot{cfg, logger};
    iot.begin();

    mock::bus = true;
    mock::busPin = 2;
    seg(HIGH, 5000);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("falha", iot.sensorText());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: leitura falhou"));
    uint8_t logged = logger.count();

    mock::busSegs.clear();
    mock::busAt = 0;
    seg(LOW, 5000);
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL(logged, logger.count());

    mock::busSegs.clear();
    mock::busAt = 0;
    seg(LOW, 80);
    seg(HIGH, 5000);
    iot.tick(IoTService::READ_GAP_MS * 2);

    mock::busSegs.clear();
    mock::busAt = 0;
    seg(LOW, 80);
    seg(HIGH, 80);
    seg(LOW, 5000);
    iot.tick(IoTService::READ_GAP_MS * 3);

    mock::busSegs.clear();
    mock::busAt = 0;
    seg(LOW, 80);
    seg(HIGH, 80);
    seg(LOW, 50);
    seg(HIGH, 5000);
    iot.tick(IoTService::READ_GAP_MS * 4);

    uint8_t bad[5] = {0x01, 0xE7, 0x01, 0x09, 0x00};
    feedDht(2, bad);
    iot.tick(IoTService::READ_GAP_MS * 5);
    TEST_ASSERT_EQUAL_STRING("falha", iot.sensorText());

    uint8_t neg[5];
    frame(0x01, 0x5E, 0x80, 0x65, neg); // 35.0% e -10.1C
    feedDht(2, neg);
    iot.tick(IoTService::READ_GAP_MS * 6);
    TEST_ASSERT_EQUAL_STRING("umid 35.0% temp -10.1C", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("[sensor] GPIO2 umid 35.0% temp -10.1C", iot.message());

    arm(2, 0);
    cfg.iot_sensor_type = IoTService::SENSOR_DHT11;
    cfg.iot_metric = 0;
    cfg.iot_above = true;
    cfg.iot_threshold = 40;
    cfg.iot_notify = false;
    IoTService dht11{cfg, logger};
    dht11.begin();
    uint8_t d11[5];
    frame(40, 0, 26, 0, d11);
    feedDht(2, d11);
    dht11.tick(0);
    TEST_ASSERT_EQUAL_STRING("umid 40.0% temp 26.0C", dht11.sensorText());
    TEST_ASSERT_TRUE(dht11.moduleOn());
    TEST_ASSERT_FALSE(dht11.pending());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: GPIO2 umid 40.0% temp 26.0C"));
}

void test_ds18b20_conversao_e_erros() {
    arm(2, 0);
    cfg.iot_sensor_type = IoTService::SENSOR_DS18B20;
    cfg.iot_above = true;
    cfg.iot_threshold = 20;
    cfg.iot_notify = true;
    IoTService iot{cfg, logger};
    iot.begin();

    const uint8_t ok[] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x33};
    feedDs(2, true, true, ok, 9);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
    iot.tick(IoTService::DS_CONVERT_MS - 1);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
    iot.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("temp 25.0C", iot.sensorText());
    TEST_ASSERT_TRUE(iot.moduleOn());
    TEST_ASSERT_TRUE(contains(std::string(iot.message()), "modulo ligado"));

    iot.tick(IoTService::READ_GAP_MS - 1);
    TEST_ASSERT_EQUAL_STRING("temp 25.0C", iot.sensorText());
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL_STRING("falha", iot.sensorText());
    TEST_ASSERT_TRUE(iot.moduleOn());

    IoTService ausente{cfg, logger};
    ausente.begin();
    feedDs(2, false, true, ok, 9);
    ausente.tick(0);
    TEST_ASSERT_EQUAL_STRING("falha", ausente.sensorText());
    uint8_t logged = logger.count();
    ausente.tick(100);
    TEST_ASSERT_EQUAL(logged, logger.count());
    ausente.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL(logged, logger.count());

    IoTService leitura{cfg, logger};
    leitura.begin();
    feedDs(2, true, false, nullptr, 0);
    leitura.tick(0);
    leitura.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("falha", leitura.sensorText());

    uint8_t ruim[] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x00};
    IoTService crc{cfg, logger};
    crc.begin();
    feedDs(2, true, true, ruim, 9);
    crc.tick(0);
    crc.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("falha", crc.sensorText());

    const uint8_t negativo[] = {0x58, 0xFF, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0xF9};
    cfg.iot_above = false;
    cfg.iot_threshold = 0;
    cfg.iot_notify = false;
    IoTService frio{cfg, logger};
    frio.begin();
    feedDs(2, true, true, negativo, 9);
    frio.tick(0);
    frio.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("temp -10.5C", frio.sensorText());
    TEST_ASSERT_TRUE(frio.moduleOn());
    TEST_ASSERT_FALSE(frio.pending());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: GPIO2 temp -10.5C"));
}

void test_pulsos_na_janela() {
    arm(2, 0);
    cfg.iot_sensor_type = IoTService::SENSOR_PULSE;
    cfg.iot_above = true;
    cfg.iot_threshold = 3;
    cfg.iot_margin = 1;
    cfg.iot_notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(2, mock::isrPin);
    TEST_ASSERT_EQUAL(FALLING, mock::isrMode);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());

    iot.tick(0);
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS - 1);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
    IoTService::onPulse();
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_EQUAL_STRING("pulsos 3", iot.sensorText());
    TEST_ASSERT_TRUE(iot.moduleOn());
    TEST_ASSERT_TRUE(contains(std::string(iot.message()), "modulo ligado"));

    IoTService::onPulse();
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 2);
    TEST_ASSERT_TRUE(iot.moduleOn());
    TEST_ASSERT_EQUAL_STRING("pulsos 2", iot.sensorText());

    iot.tick(IoTService::PULSE_WINDOW_MS * 3);
    TEST_ASSERT_FALSE(iot.moduleOn());
    TEST_ASSERT_EQUAL_STRING("pulsos 0", iot.sensorText());

    cfg.iot_sensor_pin = -1;
    iot.begin();
    TEST_ASSERT_EQUAL(255, mock::isrPin);
    iot.begin();
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.sensorText());
}

void test_histerese_abaixo_sem_ntfy() {
    arm(2, 0);
    cfg.iot_sensor_type = IoTService::SENSOR_PULSE;
    cfg.iot_above = false;
    cfg.iot_threshold = 3;
    cfg.iot_margin = 1;
    cfg.iot_notify = false;
    IoTService iot{cfg, logger};
    iot.begin();

    iot.tick(0);
    IoTService::onPulse();
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_TRUE(iot.moduleOn());
    TEST_ASSERT_EQUAL_STRING("pulsos 2", iot.sensorText());
    TEST_ASSERT_FALSE(iot.pending());
    uint8_t logged = logger.count();

    for (int i = 0; i < 4; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 2);
    TEST_ASSERT_TRUE(iot.moduleOn());
    TEST_ASSERT_EQUAL(logged, logger.count());

    for (int i = 0; i < 5; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 3);
    TEST_ASSERT_FALSE(iot.moduleOn());
    TEST_ASSERT_EQUAL_STRING("pulsos 5", iot.sensorText());
    logged = logger.count();

    for (int i = 0; i < 5; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 4);
    TEST_ASSERT_FALSE(iot.moduleOn());
    TEST_ASSERT_EQUAL(logged, logger.count());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pinos_e_parse);
    RUN_TEST(test_parse_faixa);
    RUN_TEST(test_desligado_nao_mexe_nos_pinos);
    RUN_TEST(test_so_sensor_notifica_depois_do_debounce);
    RUN_TEST(test_rele_acompanha_o_sensor);
    RUN_TEST(test_niveis_invertidos_e_sem_ntfy);
    RUN_TEST(test_mesmo_pino_nao_vira_saida);
    RUN_TEST(test_tipo_desconhecido_vira_digital);
    RUN_TEST(test_servo_acompanha_o_sensor);
    RUN_TEST(test_servo_nos_outros_pinos_e_falha_ao_anexar);
    RUN_TEST(test_dht22_umidade_modulo_e_margem);
    RUN_TEST(test_dht_falhas_temperatura_e_dht11);
    RUN_TEST(test_ds18b20_conversao_e_erros);
    RUN_TEST(test_pulsos_na_janela);
    RUN_TEST(test_histerese_abaixo_sem_ntfy);
    return UNITY_END();
}
