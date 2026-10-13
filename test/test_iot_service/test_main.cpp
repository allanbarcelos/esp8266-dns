// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "IoTService.h"

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    mock::wsPin = -1;
    mock::wsCount = 0;
    mock::wsColor = 0;
    mock::wsShows = 0;
}
void tearDown() {}

static void sensor(const char* slug, int pin) {
    strlcpy(cfg.iot.sensor, slug, sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = (int8_t)pin;
}

static void actuator(const char* slug, int pin) {
    strlcpy(cfg.iot.actuator, slug, sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = (int8_t)pin;
}

static std::string json(const IoTService& iot) {
    char buf[512];
    iot.stateJson(buf, sizeof(buf));
    return buf;
}

// Sensor digital estável no nível dado, a partir de t
static void level(IoTService& iot, int pin, int lv, unsigned long t) {
    mock::pinIn[pin] = (uint8_t)lv;
    iot.tick(t);
    iot.tick(t + IoTService::DEBOUNCE_MS);
}

static void seg(int lv, unsigned long dur) {
    mock::busSegs.push_back(mock::BusSeg{lv, dur});
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

// Leitura do DHT em dois ticks: pulso de início em t, bits em t + wake
static void dhtRead(IoTService& iot, unsigned long t,
                    unsigned long wake = IoTService::DHT22_WAKE_MS) {
    iot.tick(t);
    iot.tick(t + wake);
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

static void echo(uint8_t pin, unsigned long highUs) {
    mock::bus = true;
    mock::busPin = pin;
    mock::busAt = 0;
    mock::busSegs.clear();
    seg(HIGH, highUs);
    seg(LOW, 2000);
}

// ── Catálogo, parse e comandos de texto ──────────────────────────────────────

void test_catalogo_por_identificador() {
    TEST_ASSERT_EQUAL(9, iot::SENSOR_COUNT);
    TEST_ASSERT_EQUAL(10, iot::ACTUATOR_COUNT);
    TEST_ASSERT_EQUAL(0, iot::sensorIndex("dht22"));
    TEST_ASSERT_EQUAL(-1, iot::sensorIndex(""));
    TEST_ASSERT_EQUAL(-1, iot::sensorIndex("laser"));
    TEST_ASSERT_EQUAL(iot::C_COLOR, iot::ACTUATORS[iot::actuatorIndex("fita")].cap);
    TEST_ASSERT_EQUAL(-1, iot::actuatorIndex("teleporte"));
    TEST_ASSERT_EQUAL_STRING("pulsos", iot::v1Sensor(4));
    TEST_ASSERT_EQUAL_STRING("digital", iot::v1Sensor(200));
    TEST_ASSERT_EQUAL_STRING("motor", iot::v1Actuator(7));
    TEST_ASSERT_EQUAL_STRING("rele", iot::v1Actuator(200));
    // Toda entrada da tabela v1 existe no catálogo novo
    for (uint8_t i = 0; i < 16; i++) TEST_ASSERT_TRUE(iot::sensorIndex(iot::v1Sensor(i)) >= 0);
    for (uint8_t i = 0; i < 8; i++) TEST_ASSERT_TRUE(iot::actuatorIndex(iot::v1Actuator(i)) >= 0);
}

void test_pinos_e_parse() {
    TEST_ASSERT_TRUE(IoTService::allowedPin(0));
    TEST_ASSERT_TRUE(IoTService::allowedPin(3));
    TEST_ASSERT_FALSE(IoTService::allowedPin(-1));
    TEST_ASSERT_FALSE(IoTService::allowedPin(4));

    int8_t pin = 9;
    TEST_ASSERT_TRUE(IoTService::parsePin("-1", pin));
    TEST_ASSERT_EQUAL(-1, pin);
    TEST_ASSERT_TRUE(IoTService::parsePin("3", pin));
    TEST_ASSERT_EQUAL(3, pin);
    TEST_ASSERT_FALSE(IoTService::parsePin("", pin));
    TEST_ASSERT_FALSE(IoTService::parsePin("12", pin));
    TEST_ASSERT_FALSE(IoTService::parsePin("9", pin));
    TEST_ASSERT_FALSE(IoTService::parsePin("/", pin));

    int16_t v = 7;
    uint8_t u = 7;
    TEST_ASSERT_TRUE(IoTService::parseI16("-55", -55, 100, v));
    TEST_ASSERT_EQUAL(-55, v);
    TEST_ASSERT_FALSE(IoTService::parseI16("", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("1234567", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("-", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("1a", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("/", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("-60", -55, 100, v));
    TEST_ASSERT_FALSE(IoTService::parseI16("101", -55, 100, v));
    TEST_ASSERT_TRUE(IoTService::parseU8("4", 0, 4, u));
    TEST_ASSERT_EQUAL(4, u);
    TEST_ASSERT_FALSE(IoTService::parseU8("9", 0, 4, u));
}

static bool parse(const char* text, uint8_t op, uint32_t value = 0) {
    iot::Command c;
    if (!iot::parseCommand(text, c)) return false;
    TEST_ASSERT_EQUAL_MESSAGE(op, c.op, text);
    TEST_ASSERT_EQUAL_MESSAGE(value, c.value, text);
    return true;
}

void test_comandos_de_texto() {
    TEST_ASSERT_TRUE(parse("ligar", iot::OP_ON));
    TEST_ASSERT_TRUE(parse("  LIGAR \r\n", iot::OP_ON));
    TEST_ASSERT_TRUE(parse("abrir", iot::OP_ON));
    TEST_ASSERT_TRUE(parse("off", iot::OP_OFF));
    TEST_ASSERT_TRUE(parse("fechar", iot::OP_OFF));
    TEST_ASSERT_TRUE(parse("alternar", iot::OP_TOGGLE));
    TEST_ASSERT_TRUE(parse("pulso 5", iot::OP_PULSE, 5));
    TEST_ASSERT_TRUE(parse("pulse 3600s", iot::OP_PULSE, 3600));
    TEST_ASSERT_TRUE(parse("50%", iot::OP_LEVEL, 50));
    TEST_ASSERT_TRUE(parse("nivel   0", iot::OP_LEVEL, 0));
    TEST_ASSERT_TRUE(parse("brilho 40%", iot::OP_LEVEL, 40));
    TEST_ASSERT_TRUE(parse("n\xc3\xadvel 10", iot::OP_LEVEL, 10));
    TEST_ASSERT_TRUE(parse("\xc3\xa2ngulo 90", iot::OP_ANGLE, 90));
    TEST_ASSERT_TRUE(parse("angle 180", iot::OP_ANGLE, 180));
    TEST_ASSERT_TRUE(parse("cor #FF8800", iot::OP_COLOR, 0xFF8800));
    TEST_ASSERT_TRUE(parse("color 00ff7f", iot::OP_COLOR, 0x00FF7F));
    TEST_ASSERT_TRUE(parse("auto", iot::OP_AUTO));
    TEST_ASSERT_TRUE(parse("estado", iot::OP_STATUS));

    iot::Command c;
    const char* bad[] = {
        "", "   ", "pular", "ligar 5", "pulso", "pulso 0", "pulso 3601", "pulso 5m",
        "pulso abc", "pulso 123456", "101%", "x%", "nivel 101", "angulo 181",
        "cor", "cor #12345", "cor #1234567", "cor #12345g", "cor ff88zz",
        "cor 12345!", "cor 12345:",
        "ligar ligar ligar ligar ligar ligar ligar ligar",
    };
    for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(iot::parseCommand(b, c), b);

    TEST_ASSERT_FALSE(iot::commandAllowed(iot::C_NONE, iot::OP_ON));
    TEST_ASSERT_TRUE(iot::commandAllowed(iot::C_ONOFF, iot::OP_PULSE));
    TEST_ASSERT_FALSE(iot::commandAllowed(iot::C_ONOFF, iot::OP_LEVEL));
    TEST_ASSERT_TRUE(iot::commandAllowed(iot::C_COLOR, iot::OP_LEVEL));
    TEST_ASSERT_TRUE(iot::commandAllowed(iot::C_POSITION, iot::OP_ANGLE));
    TEST_ASSERT_FALSE(iot::commandAllowed(iot::C_LEVEL, iot::OP_ANGLE));
    TEST_ASSERT_TRUE(iot::commandAllowed(iot::C_COLOR, iot::OP_COLOR));
    TEST_ASSERT_FALSE(iot::commandAllowed(iot::C_LEVEL, iot::OP_COLOR));
}

// ── Configuração e pinos ─────────────────────────────────────────────────────

void test_desligado_nao_mexe_nos_pinos() {
    IoTService iot{cfg, logger};
    iot.begin();
    iot.tick(0);
    TEST_ASSERT_FALSE(iot.hasSensor());
    TEST_ASSERT_FALSE(iot.hasActuator());
    TEST_ASSERT_FALSE(iot.hasRule());
    TEST_ASSERT_EQUAL(iot::K_NONE, iot.sensorKind());
    TEST_ASSERT_EQUAL(iot::C_NONE, iot.capability());
    TEST_ASSERT_NULL(iot.sensorDef());
    TEST_ASSERT_NULL(iot.actuatorDef());
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("nenhum", iot.actuatorText());
    TEST_ASSERT_EQUAL(255, mock::pinModeV[2]);
    TEST_ASSERT_EQUAL(IoTService::NO_ACTUATOR, iot.command("ligar", 0));
    TEST_ASSERT_EQUAL_STRING("{\"api\":1,\"sensor\":null,\"actuator\":null}", json(iot).c_str());
    TEST_ASSERT_FALSE(iot.pending());

    // Tipo sem pino, ou pino sem tipo: nada
    sensor("pir", -1);
    actuator("", 0);
    iot.begin();
    TEST_ASSERT_FALSE(iot.hasSensor());
    TEST_ASSERT_FALSE(iot.hasActuator());
    TEST_ASSERT_FALSE(iot.unknownType());
    sensor("", 2);
    iot.begin();
    TEST_ASSERT_FALSE(iot.hasSensor());
    TEST_ASSERT_FALSE(iot.unknownType());
}

// Buffer curto: o JSON é cortado, mas sempre termina e informa o tamanho real
void test_json_em_buffer_curto() {
    sensor("dht22", 2);
    actuator("fita", 0);
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL_STRING("dht22", iot.sensorDef()->slug);
    std::string full = json(iot);
    for (size_t n : {0u, 1u, 12u, 40u, 120u}) {
        char small[128];
        std::memset(small, 'x', sizeof(small));
        size_t k = iot.stateJson(small, n);
        TEST_ASSERT_EQUAL(full.size(), k);
        if (n) TEST_ASSERT_EQUAL(n - 1, std::strlen(small));
    }
}

void test_tipo_desconhecido_desliga_a_parte() {
    sensor("laser", 2);
    actuator("teleporte", 0);
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.unknownType());
    TEST_ASSERT_FALSE(iot.hasSensor());
    TEST_ASSERT_FALSE(iot.hasActuator());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: tipo de sensor desconhecido"));
    TEST_ASSERT_TRUE(logHas(logger, "IoT: tipo de atuador desconhecido"));
    TEST_ASSERT_EQUAL(255, mock::pinModeV[0]);

    // Sensor desconhecido não ocupa o pino: o atuador pode usá-lo
    actuator("rele", 2);
    iot.begin();
    TEST_ASSERT_TRUE(iot.hasActuator());
}

void test_mesmo_pino_fica_so_com_o_sensor() {
    sensor("pir", 2);
    actuator("rele", 2);
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.hasSensor());
    TEST_ASSERT_FALSE(iot.hasActuator());
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
}

// ── Só sensor ────────────────────────────────────────────────────────────────

void test_so_sensor_publica_com_os_nomes_do_tipo() {
    sensor("contato", 2);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    mock::pinIn[2] = HIGH;
    iot.begin();
    TEST_ASSERT_TRUE(logHas(logger, "IoT: sensor contato GPIO2"));
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
    TEST_ASSERT_TRUE(contains(json(iot), "\"status\":\"waiting\""));

    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS - 1);
    TEST_ASSERT_FALSE(iot.pending());
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL_STRING("aberta", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("[sensor] aberta", iot.message());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: aberta"));
    TEST_ASSERT_EQUAL_STRING(
        "{\"api\":1,\"sensor\":{\"type\":\"contato\",\"status\":\"ok\",\"text\":\"aberta\","
        "\"active\":true},\"actuator\":null}", json(iot).c_str());
    iot.ack();
    TEST_ASSERT_FALSE(iot.pending());

    iot.tick(IoTService::DEBOUNCE_MS + 10);        // mesmo nível: nada novo
    TEST_ASSERT_FALSE(iot.pending());

    // Oscilação mais curta que o debounce não conta
    mock::pinIn[2] = LOW;
    iot.tick(1000);
    mock::pinIn[2] = HIGH;
    iot.tick(1100);
    iot.tick(1100 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL_STRING("aberta", iot.sensorText());

    level(iot, 2, LOW, 2000);
    TEST_ASSERT_EQUAL_STRING("fechada", iot.sensorText());
    TEST_ASSERT_TRUE(contains(json(iot), "\"active\":false"));
}

void test_nomes_proprios_e_nivel_invertido_sem_ntfy() {
    sensor("digital", 1);
    cfg.iot.sensorHigh = false;
    strlcpy(cfg.iot.onText, "molhado", sizeof(cfg.iot.onText));
    strlcpy(cfg.iot.offText, "seco", sizeof(cfg.iot.offText));
    IoTService iot{cfg, logger};
    iot.begin();
    level(iot, 1, LOW, 0);
    TEST_ASSERT_EQUAL_STRING("molhado", iot.sensorText());
    TEST_ASSERT_FALSE(iot.pending());
    level(iot, 1, HIGH, 1000);
    TEST_ASSERT_EQUAL_STRING("seco", iot.sensorText());
}

// ── Só atuador: capacidades e comandos ───────────────────────────────────────

void test_liga_desliga_obedece_comandos() {
    actuator("rele", 0);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(logHas(logger, "IoT: atuador rele GPIO0"));
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[0]);
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);          // liga em nível baixo
    TEST_ASSERT_EQUAL_STRING("desligado", iot.actuatorText());
    TEST_ASSERT_EQUAL_STRING("Rele", iot.actuatorDef()->label);
    TEST_ASSERT_EQUAL_STRING(
        "{\"api\":1,\"sensor\":null,\"actuator\":{\"type\":\"rele\",\"capability\":\"onoff\","
        "\"on\":false,\"mode\":\"manual\",\"text\":\"desligado\"}}", json(iot).c_str());

    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("ligar", 10));
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[0]);
    TEST_ASSERT_FALSE(iot.manual());                   // sem regra não há modo
    TEST_ASSERT_EQUAL_STRING("[atuador] ligado", iot.message());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: [atuador] ligado"));

    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("alternar", 20));
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("alternar", 30));
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("off", 40));
    TEST_ASSERT_FALSE(iot.actuatorOn());

    iot.ack();
    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("status", 50));
    TEST_ASSERT_FALSE(iot.pending());
    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("auto", 60));
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(IoTService::INVALID, iot.command("voar", 70));
    TEST_ASSERT_EQUAL(IoTService::UNSUPPORTED, iot.command("50%", 70));
    TEST_ASSERT_EQUAL(IoTService::UNSUPPORTED, iot.command("angulo 9", 70));

    // Liga em nível alto
    cfg.iot.actuatorHigh = true;
    iot.begin();
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[0]);
    iot.command("ligar", 0);
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);
}

void test_pulso_desliga_sozinho_e_nao_e_gravado() {
    actuator("fechadura", 3);
    cfg.iot.restore = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("pulso 3", 1000));
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count(IoTState::PATH));
    setMillis(1500);
    TEST_ASSERT_TRUE(contains(json(iot), "\"pulse\":3"));
    setMillis(9000);                                   // passou do fim antes do tick
    TEST_ASSERT_TRUE(contains(json(iot), "\"pulse\":0"));

    iot.tick(3999);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.tick(4000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_FALSE(contains(json(iot), "pulse"));
    IoTState st;
    TEST_ASSERT_TRUE(st.load());                       // grava o fim do pulso
    TEST_ASSERT_FALSE(st.on);

    // Outro comando cancela o pulso
    iot.command("pulso 3", 5000);
    iot.command("ligar", 5100);
    iot.tick(9000);
    TEST_ASSERT_TRUE(iot.actuatorOn());
}

void test_desligamento_de_seguranca() {
    actuator("bomba", 0);
    cfg.iot.maxOn = 1;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    iot.command("ligar", 1000);
    iot.tick(1000 + 59999);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.tick(1000 + 60000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[atuador] desligado por seguranca", iot.message());
    TEST_ASSERT_TRUE(logHas(logger, "por seguranca"));

    // Desligado não conta tempo
    iot.tick(1000 + 200000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
}

void test_intensidade_pwm() {
    actuator("ventilador", 0);
    cfg.iot.dutyOn = 80;
    cfg.iot.dutyOff = 20;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(20 * 1023 / 100, mock::analogValue[0]);
    TEST_ASSERT_EQUAL_STRING("desligado", iot.actuatorText());
    TEST_ASSERT_TRUE(contains(json(iot), "\"capability\":\"level\",\"on\":false"));
    TEST_ASSERT_TRUE(contains(json(iot), "\"level\":80"));

    iot.command("ligar", 0);
    TEST_ASSERT_EQUAL(80 * 1023 / 100, mock::analogValue[0]);
    TEST_ASSERT_EQUAL_STRING("ligado 80%", iot.actuatorText());

    iot.command("35%", 0);
    TEST_ASSERT_EQUAL(35 * 1023 / 100, mock::analogValue[0]);
    TEST_ASSERT_EQUAL(35, iot.level());

    iot.command("nivel 0", 0);                         // 0% = desligado
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("ligar", 0);                           // volta à intensidade de ligar
    TEST_ASSERT_EQUAL(80, iot.level());

    // Reconfigurar solta a saída; GPIO0 volta ao pull-up do boot
    actuator("", -1);
    iot.begin();
    TEST_ASSERT_EQUAL(0, mock::analogValue[0]);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[0]);

    // Faixas fora do limite são cortadas
    actuator("led", 1);
    cfg.iot.dutyOn = 250;
    cfg.iot.dutyOff = 250;
    iot.begin();
    TEST_ASSERT_EQUAL(1023, mock::analogValue[1]);
    actuator("", -1);
    iot.begin();
    TEST_ASSERT_EQUAL(0, mock::analogValue[1]);
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[1]);       // TX não é pino de boot
}

void test_posicao_servo() {
    actuator("servo", 2);
    cfg.iot.angleOn = 150;
    cfg.iot.angleOff = 20;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(mock::servoAttached);
    TEST_ASSERT_EQUAL(2, mock::servoPin);
    TEST_ASSERT_EQUAL(20, mock::servoAngle);
    TEST_ASSERT_EQUAL_STRING("fechado 20 graus", iot.actuatorText());
    TEST_ASSERT_TRUE(contains(json(iot), "\"angle\":20,\"ok\":true"));

    iot.command("abrir", 0);
    TEST_ASSERT_EQUAL(150, mock::servoAngle);
    TEST_ASSERT_EQUAL_STRING("aberto 150 graus", iot.actuatorText());
    iot.command("angulo 90", 0);
    TEST_ASSERT_EQUAL(90, mock::servoAngle);
    TEST_ASSERT_EQUAL(90, iot.angle());
    iot.command("angulo 20", 0);                       // o ângulo fechado é desligado
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("ligar", 0);                           // ligar volta ao aberto
    TEST_ASSERT_EQUAL(150, mock::servoAngle);
    TEST_ASSERT_EQUAL(IoTService::UNSUPPORTED, iot.command("cor #ffffff", 0));

    actuator("", -1);
    iot.begin();
    TEST_ASSERT_FALSE(mock::servoAttached);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);

    // Servo que não anexa: falha sem escrever
    mock::servoAttachResult = 0;
    mock::servoWrites = 0;
    actuator("servo", 3);
    iot.begin();
    TEST_ASSERT_TRUE(logHas(logger, "IoT: servo indisponivel"));
    TEST_ASSERT_EQUAL_STRING("falha", iot.actuatorText());
    TEST_ASSERT_TRUE(contains(json(iot), "\"ok\":false"));
    iot.command("abrir", 0);
    TEST_ASSERT_EQUAL(0, mock::servoWrites);
    actuator("", -1);
    iot.begin();
    TEST_ASSERT_EQUAL(255, mock::pinModeV[3]);         // RX não é pino de boot
}

void test_cor_fita_ws2812() {
    actuator("fita", 2);
    cfg.iot.leds = 12;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[2]);
    TEST_ASSERT_EQUAL(2, mock::wsPin);
    TEST_ASSERT_EQUAL(12, mock::wsCount);
    TEST_ASSERT_EQUAL_HEX32(0, mock::wsColor);
    TEST_ASSERT_TRUE(contains(json(iot), "\"color\":\"#ffffff\""));

    iot.command("cor #ff8800", 0);
    TEST_ASSERT_EQUAL_HEX32(0xFF8800, mock::wsColor);
    TEST_ASSERT_EQUAL_STRING("ligado #ff8800 100%", iot.actuatorText());
    TEST_ASSERT_EQUAL_HEX32(0xFF8800, iot.color());

    iot.command("brilho 50", 0);
    TEST_ASSERT_EQUAL_HEX32(0x7F4400, mock::wsColor);
    iot.command("brilho 0", 0);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_HEX32(0, mock::wsColor);
    iot.command("cor #0000ff", 0);                     // cor com brilho 0 acende a 100%
    TEST_ASSERT_EQUAL_HEX32(0x0000FF, mock::wsColor);
    iot.command("brilho 0", 0);
    iot.command("ligar", 0);                           // ligar também
    TEST_ASSERT_EQUAL(100, iot.level());

    // Quantidade fora da faixa vira o máximo; soltar apaga a fita
    cfg.iot.leds = 0;
    iot.begin();
    TEST_ASSERT_EQUAL(IoTService::MAX_LEDS, mock::wsCount);
    cfg.iot.leds = 200;
    iot.begin();
    TEST_ASSERT_EQUAL(IoTService::MAX_LEDS, mock::wsCount);
    iot.command("ligar", 0);
    actuator("", -1);
    iot.begin();
    TEST_ASSERT_EQUAL_HEX32(0, mock::wsColor);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
}

// ── Estado ao religar ────────────────────────────────────────────────────────

void test_ao_religar_volta_o_ultimo_comando() {
    actuator("fita", 1);
    cfg.iot.restore = true;
    IoTService a{cfg, logger};
    a.begin();
    a.command("cor #102030", 0);
    a.command("brilho 40", 0);

    IoTService b{cfg, logger};
    b.begin();
    TEST_ASSERT_TRUE(b.actuatorOn());
    TEST_ASSERT_EQUAL(40, b.level());
    TEST_ASSERT_EQUAL_HEX32(0x102030, b.color());

    // Valores fora da faixa no arquivo são cortados
    MockFS::put(IoTState::PATH, R"({"m":true,"on":true,"lv":150,"an":200,"c":4294967295})");
    actuator("servo", 2);
    IoTService c{cfg, logger};
    c.begin();
    TEST_ASSERT_EQUAL(180, c.angle());
    actuator("led", 0);
    IoTService d{cfg, logger};
    d.begin();
    TEST_ASSERT_EQUAL(100, d.level());

    // Arquivo estragado ou ausente: desligado
    MockFS::put(IoTState::PATH, "{");
    IoTService e{cfg, logger};
    e.begin();
    TEST_ASSERT_FALSE(e.actuatorOn());
    MockFS::reset();
    IoTService f{cfg, logger};
    f.begin();
    TEST_ASSERT_FALSE(f.actuatorOn());

    // Sem a opção, não grava nem lê
    cfg.iot.restore = false;
    IoTService g{cfg, logger};
    g.begin();
    g.command("ligar", 0);
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count(IoTState::PATH));

    // Falha ao gravar não derruba o comando
    cfg.iot.restore = true;
    MockFS::failWriteOpen = true;
    IoTService h{cfg, logger};
    h.begin();
    TEST_ASSERT_EQUAL(IoTService::OK, h.command("ligar", 0));
}

// ── Sensor e atuador: regra ──────────────────────────────────────────────────

void test_regra_digital_acompanha_e_manual_tem_prioridade() {
    sensor("pir", 2);
    actuator("rele", 0);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.hasRule());
    TEST_ASSERT_TRUE(contains(json(iot), "\"mode\":\"auto\""));

    level(iot, 2, HIGH, 0);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[0]);
    TEST_ASSERT_EQUAL_STRING("[sensor] presenca | ligado", iot.message());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: presenca | ligado"));

    // Comando manual: a regra não mexe até "auto"
    iot.command("desligar", 500);
    TEST_ASSERT_TRUE(iot.manual());
    TEST_ASSERT_TRUE(contains(json(iot), "\"mode\":\"manual\""));
    level(iot, 2, LOW, 1000);
    level(iot, 2, HIGH, 2000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[sensor] presenca | desligado", iot.message());

    iot.command("auto", 3000);                         // volta e aplica a regra
    TEST_ASSERT_FALSE(iot.manual());
    TEST_ASSERT_TRUE(iot.actuatorOn());

    // Pulso em regra: ao terminar, volta ao automático
    level(iot, 2, LOW, 4000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("pulso 1", 5000);
    TEST_ASSERT_TRUE(iot.manual());
    iot.tick(6000);
    TEST_ASSERT_FALSE(iot.manual());
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // Segurança em regra: fica no manual (a regra religaria na hora)
    cfg.iot.maxOn = 1;
    iot.begin();
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.tick(60000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(iot.manual());
}

void test_auto_antes_da_primeira_leitura_e_regra_desligada() {
    sensor("botao", 2);
    actuator("rele", 0);
    IoTService iot{cfg, logger};
    iot.begin();
    iot.command("ligar", 0);
    iot.command("auto", 0);                            // a regra ainda não decidiu
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.command("pulso 1", 0);
    iot.tick(1000);                                    // fim do pulso sem decisão
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // Sem a regra, o sensor só informa
    cfg.iot.rule = false;
    iot.begin();
    TEST_ASSERT_FALSE(iot.hasRule());
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(json(iot), "\"mode\":\"manual\""));
}

void test_regra_digital_alterna_a_cada_acionamento() {
    sensor("botao", 3);
    actuator("rele", 0);
    cfg.iot.toggle = true;
    IoTService iot{cfg, logger};
    iot.begin();
    level(iot, 3, HIGH, 0);                            // 1º estado só arma
    TEST_ASSERT_FALSE(iot.actuatorOn());
    level(iot, 3, LOW, 1000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    level(iot, 3, HIGH, 2000);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    level(iot, 3, LOW, 3000);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    level(iot, 3, HIGH, 4000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
}

void test_regra_dht22_com_margem() {
    sensor("dht22", 2);
    actuator("rele", 0);
    cfg.iot.above = true;
    cfg.iot.threshold = 40;
    cfg.iot.margin = 2;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());

    uint8_t d[5];
    frame(0x01, 0xE7, 0x01, 0x09, d);                  // 48.7% e 26.5 C
    feedDht(2, d);
    dhtRead(iot, 0);
    TEST_ASSERT_EQUAL_STRING("26.5 C, 48.7%", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[sensor] 26.5 C, 48.7% | ligado", iot.message());
    TEST_ASSERT_TRUE(contains(json(iot), "\"temperature\":26.5,\"humidity\":48.7"));
    uint8_t logged = logger.count();

    iot.tick(IoTService::READ_GAP_MS - 1);             // ainda no intervalo
    feedDht(2, d);
    dhtRead(iot, IoTService::READ_GAP_MS);              // igual: não registra
    TEST_ASSERT_EQUAL(logged, logger.count());

    frame(0x01, 0x81, 0x01, 0x09, d);                  // 38.5%: dentro da margem
    feedDht(2, d);
    dhtRead(iot, IoTService::READ_GAP_MS * 2);
    TEST_ASSERT_TRUE(iot.actuatorOn());

    frame(0x01, 0x72, 0x01, 0x09, d);                  // 37.0%: desliga
    feedDht(2, d);
    dhtRead(iot, IoTService::READ_GAP_MS * 3);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(logHas(logger, "| desligado"));
}

void test_dht_falhas_temperatura_negativa_e_dht11() {
    sensor("dht22", 2);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();

    mock::bus = true;
    mock::busPin = 2;
    seg(HIGH, 5000);
    dhtRead(iot, 0);
    TEST_ASSERT_EQUAL_STRING("falha", iot.sensorText());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: leitura falhou"));
    TEST_ASSERT_TRUE(contains(json(iot), "\"status\":\"error\",\"text\":\"falha\"}"));
    uint8_t logged = logger.count();

    mock::busSegs.clear();
    mock::busAt = 0;
    seg(LOW, 5000);
    dhtRead(iot, IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL(logged, logger.count());

    mock::busSegs.clear();
    mock::busAt = 0;
    seg(LOW, 80);
    seg(HIGH, 5000);
    dhtRead(iot, IoTService::READ_GAP_MS * 2);

    mock::busSegs.clear();
    mock::busAt = 0;
    seg(LOW, 80);
    seg(HIGH, 80);
    seg(LOW, 5000);
    dhtRead(iot, IoTService::READ_GAP_MS * 3);

    mock::busSegs.clear();
    mock::busAt = 0;
    seg(LOW, 80);
    seg(HIGH, 80);
    seg(LOW, 50);
    seg(HIGH, 5000);
    dhtRead(iot, IoTService::READ_GAP_MS * 4);

    uint8_t bad[5] = {0x01, 0xE7, 0x01, 0x09, 0x00};
    feedDht(2, bad);
    dhtRead(iot, IoTService::READ_GAP_MS * 5);
    TEST_ASSERT_EQUAL_STRING("falha", iot.sensorText());

    uint8_t neg[5];
    frame(0x01, 0x5E, 0x80, 0x65, neg);                // 35.0% e -10.1 C
    feedDht(2, neg);
    dhtRead(iot, IoTService::READ_GAP_MS * 6);
    TEST_ASSERT_EQUAL_STRING("-10.1 C, 35.0%", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("[sensor] -10.1 C, 35.0%", iot.message());

    sensor("dht11", 2);
    actuator("rele", 0);
    cfg.iot.metric = 1;
    cfg.iot.above = true;
    cfg.iot.threshold = 25;
    cfg.iot.notify = false;
    IoTService dht11{cfg, logger};
    dht11.begin();
    uint8_t d11[5];
    frame(40, 0, 26, 0, d11);
    feedDht(2, d11);
    dhtRead(dht11, 0, IoTService::DHT11_WAKE_MS);
    TEST_ASSERT_EQUAL_STRING("26.0 C, 40.0%", dht11.sensorText());
    TEST_ASSERT_TRUE(dht11.actuatorOn());               // temperatura 26 > 25
    TEST_ASSERT_FALSE(dht11.pending());
}

// O pulso de início do DHT não usa delay: o pino fica em LOW entre dois
// ticks. Antes do mínimo não lê; depois do máximo recomeça o pulso.
void test_dht_pulso_de_inicio_sem_delay() {
    const struct { const char* slug; unsigned long wake, max; } cases[] = {
        {"dht11", IoTService::DHT11_WAKE_MS, IoTService::DHT11_WAKE_MAX},
        {"dht22", IoTService::DHT22_WAKE_MS, IoTService::DHT22_WAKE_MAX},
    };
    for (const auto& c : cases) {
        setUp();
        sensor(c.slug, 2);
        IoTService iot{cfg, logger};
        iot.begin();
        uint8_t d[5];
        frame(40, 0, 26, 0, d);
        feedDht(2, d);

        unsigned long before = micros();
        iot.tick(100);
        TEST_ASSERT_EQUAL(before, micros());          // não esperou nada
        TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[2]);
        TEST_ASSERT_EQUAL(LOW, mock::pinOut[2]);

        iot.tick(100 + c.wake - 1);                    // ainda no pulso
        TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());

        iot.tick(100 + c.max + 1);                     // loop atrasou: recomeça
        TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
        TEST_ASSERT_EQUAL(LOW, mock::pinOut[2]);

        iot.tick(100 + c.max + 1 + c.wake);            // janela certa: lê
        TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
        TEST_ASSERT_TRUE(contains(std::string(iot.sensorText()), " C, "));
    }
}

void test_ds18b20_conversao_e_erros() {
    sensor("ds18b20", 2);
    actuator("rele", 0);
    cfg.iot.above = true;
    cfg.iot.threshold = 20;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();

    const uint8_t ok[] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x33};
    feedDs(2, true, true, ok, 9);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
    iot.tick(IoTService::DS_CONVERT_MS - 1);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
    iot.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("25.0 C", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(json(iot), "\"temperature\":25.0}"));

    iot.tick(IoTService::READ_GAP_MS - 1);
    TEST_ASSERT_EQUAL_STRING("25.0 C", iot.sensorText());
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL_STRING("falha", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());

    IoTService ausente{cfg, logger};
    ausente.begin();
    feedDs(2, false, true, ok, 9);
    ausente.tick(0);
    TEST_ASSERT_EQUAL_STRING("falha", ausente.sensorText());
    uint8_t logged = logger.count();
    ausente.tick(100);
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
    cfg.iot.above = false;
    cfg.iot.threshold = 0;
    cfg.iot.notify = false;
    IoTService frio{cfg, logger};
    frio.begin();
    feedDs(2, true, true, negativo, 9);
    frio.tick(0);
    frio.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("-10.5 C", frio.sensorText());
    TEST_ASSERT_TRUE(frio.actuatorOn());
    TEST_ASSERT_FALSE(frio.pending());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: -10.5 C | ligado"));
}

void test_pulsos_na_janela_com_histerese() {
    sensor("pulsos", 2);
    actuator("rele", 0);
    cfg.iot.above = true;
    cfg.iot.threshold = 3;
    cfg.iot.margin = 1;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(2, mock::isrPin);
    TEST_ASSERT_EQUAL(FALLING, mock::isrMode);

    iot.tick(0);
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS - 1);
    TEST_ASSERT_EQUAL_STRING("aguardando", iot.sensorText());
    IoTService::onPulse();
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_EQUAL_STRING("3 pulsos/10 s", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(json(iot), "\"pulses\":3}"));

    IoTService::onPulse();
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 2);         // 2 ≥ 3 - 1: segue ligado
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.tick(IoTService::PULSE_WINDOW_MS * 3);
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // Abaixo do limite, com a margem para cima
    cfg.iot.above = false;
    iot.begin();
    iot.tick(0);
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    for (int i = 0; i < 4; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 2);         // 4 ≤ 3 + 1
    TEST_ASSERT_TRUE(iot.actuatorOn());
    for (int i = 0; i < 5; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 3);
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // Trocar de sensor solta a interrupção
    sensor("pir", 2);
    iot.begin();
    TEST_ASSERT_EQUAL(255, mock::isrPin);
}

// ── Janela de horário ────────────────────────────────────────────────────────

static time_t clockNow = 0;
static time_t fakeClock() { return clockNow; }
static const time_t DAY0 = 1704067200;   // 01/01/2024 00:00 UTC
static void at(int h, int m) { clockNow = DAY0 + h * 3600 + m * 60; }

void test_hora_local_e_janela() {
    uint16_t m = 0;
    IoTService none{cfg, logger};
    TEST_ASSERT_FALSE(none.localMinute(m));

    Clock clock{fakeClock};
    IoTService iot{cfg, logger, &clock};
    clockNow = 1000;                                   // 1970
    TEST_ASSERT_FALSE(iot.localMinute(m));
    TEST_ASSERT_TRUE(iot.inHours());                   // janela desligada

    at(2, 30);
    cfg.tz = -180;                                     // volta para o dia anterior
    TEST_ASSERT_TRUE(iot.localMinute(m));
    TEST_ASSERT_EQUAL(23 * 60 + 30, m);
    cfg.tz = 330;
    TEST_ASSERT_TRUE(iot.localMinute(m));
    TEST_ASSERT_EQUAL(8 * 60, m);
    at(22, 0);
    cfg.tz = 14 * 60;                                  // passa para o dia seguinte
    TEST_ASSERT_TRUE(iot.localMinute(m));
    TEST_ASSERT_EQUAL(12 * 60, m);

    cfg.tz = 0;
    cfg.iot.window = true;
    cfg.iot.winStart = 8 * 60;
    cfg.iot.winEnd = 18 * 60;
    iot.begin();
    struct { int h, m; bool in; } day[] = {{7, 59, false}, {8, 0, true}, {17, 59, true}, {18, 0, false}};
    for (auto& c : day) {
        at(c.h, c.m);
        TEST_ASSERT_EQUAL(c.in, iot.inHours());
    }
    cfg.iot.winStart = 22 * 60;                        // vira a meia-noite
    cfg.iot.winEnd = 6 * 60;
    iot.begin();
    struct { int h, m; bool in; } night[] = {{21, 59, false}, {22, 0, true}, {23, 0, true},
                                             {5, 59, true}, {6, 0, false}};
    for (auto& c : night) {
        at(c.h, c.m);
        TEST_ASSERT_EQUAL(c.in, iot.inHours());
    }
    clockNow = 1000;
    TEST_ASSERT_FALSE(iot.inHours());                  // relógio em 1970: fora
}

void test_regra_so_dentro_do_horario() {
    sensor("pir", 2);
    actuator("rele", 0);
    cfg.iot.notify = true;
    cfg.iot.window = true;
    cfg.iot.winStart = 8 * 60;
    cfg.iot.winEnd = 18 * 60;
    Clock clock{fakeClock};
    at(12, 0);
    IoTService iot{cfg, logger, &clock};
    iot.begin();
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(json(iot), "\"hours\":true"));

    at(18, 0);
    iot.tick(1000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_FALSE(iot.manual());
    TEST_ASSERT_EQUAL_STRING("[atuador] desligado (fim do horario)", iot.message());
    TEST_ASSERT_TRUE(contains(json(iot), "\"mode\":\"auto\",\"text\":\"desligado\",\"hours\":false"));
    level(iot, 2, LOW, 2000);
    level(iot, 2, HIGH, 3000);                         // a regra pede, o horário não deixa
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // Comando manual vale fora do horário; "auto" volta a desligar
    iot.command("ligar", 4000);
    iot.tick(5000);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.command("auto", 6000);
    TEST_ASSERT_FALSE(iot.actuatorOn());

    at(8, 0);
    iot.tick(7000);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[atuador] ligado (inicio do horario)", iot.message());

    // Em manual, o horário não mexe
    iot.command("desligar", 8000);
    at(19, 0);
    iot.tick(9000);
    at(9, 0);
    iot.tick(10000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(iot.manual());

    // Sem a janela, o JSON não traz "hours"
    cfg.iot.window = false;
    iot.begin();
    TEST_ASSERT_FALSE(contains(json(iot), "hours"));

    // Só atuador: a janela não tem regra para cercar
    cfg.iot.window = true;
    cfg.iot.sensorPin = -1;
    iot.begin();
    iot.command("ligar", 0);
    at(20, 0);
    iot.tick(100);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_FALSE(contains(json(iot), "hours"));
}

// Fora do horário, "auto" e o fim do pulso desligam mesmo sem leitura
void test_fora_do_horario_sem_leitura() {
    sensor("botao", 2);
    actuator("rele", 0);
    cfg.iot.window = true;
    cfg.iot.winStart = 8 * 60;
    cfg.iot.winEnd = 9 * 60;
    Clock clock{fakeClock};
    at(20, 0);
    IoTService iot{cfg, logger, &clock};
    iot.begin();
    iot.tick(0);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("ligar", 0);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.command("auto", 10);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("pulso 1", 20);
    iot.tick(1020);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_FALSE(iot.manual());
}

// Intensidade proporcional: fora do horário, a intensidade de desligado
void test_proporcional_fora_do_horario() {
    sensor("pulsos", 2);
    actuator("ventilador", 0);
    cfg.iot.linear = true;
    cfg.iot.threshold = 0;
    cfg.iot.thresholdHi = 10;
    cfg.iot.dutyOn = 100;
    cfg.iot.dutyOff = 20;
    cfg.iot.window = true;
    cfg.iot.winStart = 8 * 60;
    cfg.iot.winEnd = 18 * 60;
    Clock clock{fakeClock};
    at(10, 0);
    IoTService iot{cfg, logger, &clock};
    iot.begin();
    iot.tick(0);
    for (int i = 0; i < 5; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(60, iot.level());

    at(18, 30);
    iot.tick(IoTService::PULSE_WINDOW_MS + 1);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(20, iot.level());
    TEST_ASSERT_EQUAL(20 * 1023 / 100, mock::analogValue[0]);
    at(9, 0);
    iot.tick(IoTService::PULSE_WINDOW_MS + 2);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(60, iot.level());
}

void test_ultrassom_distancia_e_falhas() {
    sensor("distancia", 2);
    actuator("rele", 0);
    cfg.iot.threshold = 10;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();

    echo(2, 580);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("10 cm", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(json(iot), "\"distance\":10}"));

    echo(2, 0);
    iot.tick(IoTService::READ_GAP_MS - 1);
    TEST_ASSERT_EQUAL_STRING("10 cm", iot.sensorText());

    echo(2, 15 * 58);
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_FALSE(iot.actuatorOn());

    echo(2, 2);
    iot.tick(IoTService::READ_GAP_MS * 2);
    TEST_ASSERT_EQUAL_STRING("falha", iot.sensorText());

    mock::busAt = 0;
    mock::busSegs.clear();
    seg(LOW, 80000);
    iot.tick(IoTService::READ_GAP_MS * 3);
    mock::busAt = 0;
    mock::busSegs.clear();
    seg(HIGH, 80000);
    iot.tick(IoTService::READ_GAP_MS * 4);
    echo(2, 401 * 58);
    iot.tick(IoTService::READ_GAP_MS * 5);
    TEST_ASSERT_EQUAL_STRING("falha", iot.sensorText());

    echo(2, 400 * 58);
    iot.tick(IoTService::READ_GAP_MS * 6);
    TEST_ASSERT_EQUAL_STRING("400 cm", iot.sensorText());
}

void test_intensidade_proporcional() {
    sensor("ds18b20", 2);
    actuator("ventilador", 0);
    cfg.iot.linear = true;
    cfg.iot.threshold = 20;
    cfg.iot.thresholdHi = 30;
    cfg.iot.dutyOn = 100;
    cfg.iot.dutyOff = 0;
    IoTService iot{cfg, logger};
    iot.begin();

    struct { uint8_t lo, hi, crc; int duty; } reads[] = {
        {0x40, 0x01, 0, 0},     // 20.0 C: no limite, desligado
        {0x90, 0x01, 0, 50},    // 25.0 C: meio
        {0xF0, 0x01, 0, 100},   // 31.0 C: acima do limite alto
        {0x90, 0x01, 0, 50},
        {0x90, 0x01, 0, 50},    // igual: nada muda
    };
    unsigned long t = 0;
    for (auto& r : reads) {
        uint8_t b[9] = {r.lo, r.hi, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0};
        uint8_t crc = 0;
        for (int i = 0; i < 8; i++) {
            uint8_t in = b[i];
            for (int k = 0; k < 8; k++) {
                uint8_t mix = (uint8_t)((crc ^ in) & 1);
                crc = (uint8_t)(crc >> 1);
                if (mix) crc ^= 0x8C;
                in >>= 1;
            }
        }
        b[8] = crc;
        feedDs(2, true, true, b, 9);
        iot.tick(t);
        iot.tick(t + IoTService::DS_CONVERT_MS);
        TEST_ASSERT_EQUAL(r.duty * 1023 / 100, mock::analogValue[0]);
        t += IoTService::READ_GAP_MS;
    }
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(50, iot.level());

    // Faixa vazia: dois patamares
    cfg.iot.thresholdHi = 20;
    iot.begin();
    uint8_t b25[] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x33};
    feedDs(2, true, true, b25, 9);
    iot.tick(0);
    iot.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL(1023, mock::analogValue[0]);
    uint8_t b10[] = {0xA0, 0x00, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x00};
    uint8_t crc = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t in = b10[i];
        for (int k = 0; k < 8; k++) {
            uint8_t mix = (uint8_t)((crc ^ in) & 1);
            crc = (uint8_t)(crc >> 1);
            if (mix) crc ^= 0x8C;
            in >>= 1;
        }
    }
    b10[8] = crc;
    feedDs(2, true, true, b10, 9);
    iot.tick(IoTService::READ_GAP_MS);
    iot.tick(IoTService::READ_GAP_MS + IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL(0, mock::analogValue[0]);

    // Regra comum com intensidade: ligar usa a intensidade de ligar
    cfg.iot.linear = false;
    cfg.iot.above = true;
    cfg.iot.dutyOn = 60;
    iot.begin();
    iot.command("nivel 10", 0);
    iot.command("desligar", 0);
    iot.command("auto", 0);
    feedDs(2, true, true, b25, 9);
    iot.tick(0);
    iot.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL(60, iot.level());

    // Proporcional não vale para sensor digital
    sensor("pir", 2);
    cfg.iot.linear = true;
    iot.begin();
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_EQUAL(60 * 1023 / 100, mock::analogValue[0]);
}

// Distância e pulsos não têm décimos; proporcional pedido para relé é ignorado
void test_proporcional_sem_decimos_e_manual_numerico() {
    sensor("distancia", 2);
    actuator("led", 0);
    cfg.iot.linear = true;
    cfg.iot.threshold = 10;
    cfg.iot.thresholdHi = 30;
    IoTService iot{cfg, logger};
    iot.begin();
    echo(2, 20 * 58);
    iot.tick(0);
    TEST_ASSERT_EQUAL(50, iot.level());

    // Manual: a leitura muda, a saída não
    iot.command("desligar", 0);
    echo(2, 30 * 58);
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("30 cm", iot.sensorText());

    actuator("rele", 0);
    cfg.iot.above = true;
    iot.begin();
    echo(2, 20 * 58);
    iot.tick(0);
    TEST_ASSERT_TRUE(iot.actuatorOn());                // 20 ≥ 10: regra comum
}

void test_regra_abre_o_servo() {
    sensor("pir", 2);
    actuator("servo", 0);
    cfg.iot.angleOn = 120;
    IoTService iot{cfg, logger};
    iot.begin();
    iot.command("angulo 45", 0);
    iot.command("fechar", 0);
    iot.command("auto", 0);
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_EQUAL(120, mock::servoAngle);
}

// Aviso urgente segue urgente até sair, mesmo com leituras no meio
void test_urgente_se_acumula_ate_o_envio() {
    sensor("pir", 2);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_TRUE(iot.urgent());
    level(iot, 2, LOW, 1000);
    TEST_ASSERT_TRUE(iot.urgent());
    TEST_ASSERT_EQUAL_STRING("[sensor] livre", iot.message());
    iot.ack();
    TEST_ASSERT_FALSE(iot.urgent());
    TEST_ASSERT_FALSE(iot.pending());
}

// O maior estado gravado cabe no texto lido; arquivo maior é ignorado
void test_estado_no_limite_do_texto() {
    IoTState st;
    st.manual = false;
    st.on     = false;
    st.level  = 255;
    st.angle  = 255;
    st.rgb    = 0xFFFFFFFFUL;
    TEST_ASSERT_TRUE(st.save());
    TEST_ASSERT_EQUAL(55, (int)MockFS::files[IoTState::PATH].size());
    TEST_ASSERT_TRUE(MockFS::files[IoTState::PATH].size() < IoTState::MAX_TEXT);
    IoTState back;
    TEST_ASSERT_TRUE(back.load());
    TEST_ASSERT_EQUAL(255, back.level);
    TEST_ASSERT_EQUAL(255, back.angle);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFUL, back.rgb);

    // Maior que o texto: cortado, JSON inválido, padrão
    std::string big = R"({"m":true,"on":true,"lv":40,"an":10,"c":1,"x":")";
    big += std::string(IoTState::MAX_TEXT, 'x') + "\"}";
    MockFS::put(IoTState::PATH, big);
    IoTState cut;
    TEST_ASSERT_FALSE(cut.load());
    TEST_ASSERT_FALSE(cut.on);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_catalogo_por_identificador);
    RUN_TEST(test_pinos_e_parse);
    RUN_TEST(test_comandos_de_texto);
    RUN_TEST(test_desligado_nao_mexe_nos_pinos);
    RUN_TEST(test_json_em_buffer_curto);
    RUN_TEST(test_tipo_desconhecido_desliga_a_parte);
    RUN_TEST(test_mesmo_pino_fica_so_com_o_sensor);
    RUN_TEST(test_so_sensor_publica_com_os_nomes_do_tipo);
    RUN_TEST(test_nomes_proprios_e_nivel_invertido_sem_ntfy);
    RUN_TEST(test_liga_desliga_obedece_comandos);
    RUN_TEST(test_pulso_desliga_sozinho_e_nao_e_gravado);
    RUN_TEST(test_desligamento_de_seguranca);
    RUN_TEST(test_intensidade_pwm);
    RUN_TEST(test_posicao_servo);
    RUN_TEST(test_cor_fita_ws2812);
    RUN_TEST(test_ao_religar_volta_o_ultimo_comando);
    RUN_TEST(test_regra_digital_acompanha_e_manual_tem_prioridade);
    RUN_TEST(test_auto_antes_da_primeira_leitura_e_regra_desligada);
    RUN_TEST(test_regra_digital_alterna_a_cada_acionamento);
    RUN_TEST(test_regra_dht22_com_margem);
    RUN_TEST(test_dht_falhas_temperatura_negativa_e_dht11);
    RUN_TEST(test_dht_pulso_de_inicio_sem_delay);
    RUN_TEST(test_ds18b20_conversao_e_erros);
    RUN_TEST(test_pulsos_na_janela_com_histerese);
    RUN_TEST(test_hora_local_e_janela);
    RUN_TEST(test_regra_so_dentro_do_horario);
    RUN_TEST(test_fora_do_horario_sem_leitura);
    RUN_TEST(test_proporcional_fora_do_horario);
    RUN_TEST(test_ultrassom_distancia_e_falhas);
    RUN_TEST(test_intensidade_proporcional);
    RUN_TEST(test_proporcional_sem_decimos_e_manual_numerico);
    RUN_TEST(test_regra_abre_o_servo);
    RUN_TEST(test_urgente_se_acumula_ate_o_envio);
    RUN_TEST(test_estado_no_limite_do_texto);
    return UNITY_END();
}
