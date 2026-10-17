// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "IoTService.h"
#include "../i2c_sim.h"

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

static std::string stateOf(const IoTService& iot) {
    char buf[512];
    iot.stateJson(buf, sizeof(buf));
    return buf;
}

// Digital sensor steady at the given level, from t
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

// DHT reading over two ticks: start pulse at t, bits at t + wake
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

// ── Catalog, parsing and text commands ───────────────────────────────────────

void test_catalog_by_identifier() {
    TEST_ASSERT_EQUAL(17, iot::SENSOR_COUNT);
    TEST_ASSERT_TRUE(iot::tempAndHumidity(iot::K_SHT3X) && !iot::tempAndHumidity(iot::K_DS18));
    TEST_ASSERT_TRUE(iot::tempAndHumidity(iot::K_AHT) && !iot::tempAndHumidity(iot::K_LUX));
    TEST_ASSERT_EQUAL(11, iot::ACTUATOR_COUNT);
    // I2C: SHT3x, AHT20 and BH1750 only
    for (uint8_t k = iot::K_NONE; k <= iot::K_LUX; k++)
        TEST_ASSERT_EQUAL(k == iot::K_SHT3X || k == iot::K_AHT || k == iot::K_LUX, iot::i2c(k));
    // Units offered: none for digital and light, three for distance and pulses
    const uint8_t units[] = {2, 0, 2, 2, 2, 3, 3, 2, 2, 0};
    for (uint8_t k = iot::K_NONE; k <= iot::K_LUX; k++) TEST_ASSERT_EQUAL(units[k], iot::unitCount(k));
    // The named digital sensors, with their state names; the gate pulses for 1 s
    const char* named[][3] = {{"radar", "presence", "clear"}, {"leak", "wet", "dry"}, {"gas", "gas", "clear"},
                              {"float", "high", "low"}, {"rain", "rain", "dry"}};
    for (auto& n : named) {
        int8_t i = iot::sensorIndex(n[0]);
        TEST_ASSERT_TRUE(i >= 0);
        TEST_ASSERT_EQUAL(iot::K_DIGITAL, iot::SENSORS[i].kind);
        TEST_ASSERT_EQUAL_STRING(n[1], iot::SENSORS[i].on);
        TEST_ASSERT_EQUAL_STRING(n[2], iot::SENSORS[i].off);
    }
    TEST_ASSERT_EQUAL(iot::K_AHT, iot::SENSORS[iot::sensorIndex("aht20")].kind);
    TEST_ASSERT_EQUAL(iot::K_LUX, iot::SENSORS[iot::sensorIndex("bh1750")].kind);
    TEST_ASSERT_EQUAL(1, iot::ACTUATORS[iot::actuatorIndex("gate")].pulse);
    TEST_ASSERT_EQUAL(0, iot::sensorIndex("dht22"));
    TEST_ASSERT_EQUAL(-1, iot::sensorIndex(""));
    TEST_ASSERT_EQUAL(-1, iot::sensorIndex("laser"));
    TEST_ASSERT_EQUAL(iot::C_COLOR, iot::ACTUATORS[iot::actuatorIndex("strip")].cap);
    TEST_ASSERT_EQUAL(-1, iot::actuatorIndex("teleporte"));
    TEST_ASSERT_EQUAL_STRING("pulses", iot::v1Sensor(4));
    TEST_ASSERT_EQUAL_STRING("digital", iot::v1Sensor(200));
    TEST_ASSERT_EQUAL_STRING("motor", iot::v1Actuator(7));
    TEST_ASSERT_EQUAL_STRING("relay", iot::v1Actuator(200));
    // Every entry of the v1 table exists in the new catalog
    for (uint8_t i = 0; i < 16; i++) TEST_ASSERT_TRUE(iot::sensorIndex(iot::v1Sensor(i)) >= 0);
    for (uint8_t i = 0; i < 8; i++) TEST_ASSERT_TRUE(iot::actuatorIndex(iot::v1Actuator(i)) >= 0);
}

void test_pins_and_parse() {
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

void test_text_commands() {
    TEST_ASSERT_TRUE(parse("on", iot::OP_ON));
    TEST_ASSERT_TRUE(parse("  ON \r\n", iot::OP_ON));
    TEST_ASSERT_TRUE(parse("open", iot::OP_ON));
    TEST_ASSERT_TRUE(parse("off", iot::OP_OFF));
    TEST_ASSERT_TRUE(parse("close", iot::OP_OFF));
    TEST_ASSERT_TRUE(parse("toggle", iot::OP_TOGGLE));
    TEST_ASSERT_TRUE(parse("pulse 5", iot::OP_PULSE, 5));
    TEST_ASSERT_TRUE(parse("pulse 3600s", iot::OP_PULSE, 3600));
    TEST_ASSERT_TRUE(parse("50%", iot::OP_LEVEL, 50));
    TEST_ASSERT_TRUE(parse("level   0", iot::OP_LEVEL, 0));
    TEST_ASSERT_TRUE(parse("brightness 40%", iot::OP_LEVEL, 40));
    TEST_ASSERT_TRUE(parse("LEVEL 10%", iot::OP_LEVEL, 10));
    TEST_ASSERT_TRUE(parse("Angle 90", iot::OP_ANGLE, 90));
    TEST_ASSERT_TRUE(parse("angle 180", iot::OP_ANGLE, 180));
    TEST_ASSERT_TRUE(parse("color #FF8800", iot::OP_COLOR, 0xFF8800));
    TEST_ASSERT_TRUE(parse("color 00ff7f", iot::OP_COLOR, 0x00FF7F));
    TEST_ASSERT_TRUE(parse("auto", iot::OP_AUTO));
    TEST_ASSERT_TRUE(parse("status", iot::OP_STATUS));

    iot::Command c;
    const char* bad[] = {
        "", "   ", "jump", "on 5", "pulse", "pulse 0", "pulse 3601", "pulse 5m",
        "pulse abc", "pulse 123456", "101%", "x%", "level 101", "angle 181",
        "color", "color #12345", "color #1234567", "color #12345g", "color ff88zz",
        "color 12345!", "color 12345:",
        "on on on on on on on on",
        "toggle toggle toggle toggle toggle toggle toggle",   // longer than the buffer
        // English only: the old Portuguese words are gone
        "ligar", "desligar", "pulso 5", "nivel 50", "angulo 90", "cor #ff8800", "estado",
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

// ── Configuration and pins ───────────────────────────────────────────────────

void test_off_leaves_pins_alone() {
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
    TEST_ASSERT_EQUAL_STRING("none", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("none", iot.actuatorText());
    TEST_ASSERT_EQUAL(255, mock::pinModeV[2]);
    TEST_ASSERT_EQUAL(IoTService::NO_ACTUATOR, iot.command("on", 0));
    TEST_ASSERT_EQUAL_STRING("{\"api\":1,\"sensor\":null,\"actuator\":null}", stateOf(iot).c_str());
    TEST_ASSERT_FALSE(iot.pending());

    // Type without a pin, or pin without a type: nothing
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

// Short buffer: the JSON is cut, but it always ends and reports the real length
void test_json_in_short_buffer() {
    sensor("dht22", 2);
    actuator("strip", 0);
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL_STRING("dht22", iot.sensorDef()->slug);
    std::string full = stateOf(iot);
    for (size_t n : {0u, 1u, 12u, 40u, 120u}) {
        char small[128];
        std::memset(small, 'x', sizeof(small));
        size_t k = iot.stateJson(small, n);
        TEST_ASSERT_EQUAL(full.size(), k);
        if (n) TEST_ASSERT_EQUAL(n - 1, std::strlen(small));
    }
}

void test_unknown_type_turns_part_off() {
    sensor("laser", 2);
    actuator("teleporte", 0);
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.unknownType());
    TEST_ASSERT_FALSE(iot.hasSensor());
    TEST_ASSERT_FALSE(iot.hasActuator());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: unknown sensor type"));
    TEST_ASSERT_TRUE(logHas(logger, "IoT: unknown actuator type"));
    TEST_ASSERT_EQUAL(255, mock::pinModeV[0]);

    // An unknown sensor does not hold the pin: the actuator may use it
    actuator("relay", 2);
    iot.begin();
    TEST_ASSERT_TRUE(iot.hasActuator());
}

void test_same_pin_stays_with_sensor() {
    sensor("pir", 2);
    actuator("relay", 2);
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.hasSensor());
    TEST_ASSERT_FALSE(iot.hasActuator());
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
}

// ── Sensor only ──────────────────────────────────────────────────────────────

void test_sensor_only_publishes_with_type_names() {
    sensor("contact", 2);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    mock::pinIn[2] = HIGH;
    iot.begin();
    TEST_ASSERT_TRUE(logHas(logger, "IoT: sensor contact GPIO2"));
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
    TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"status\":\"waiting\""));

    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS - 1);
    TEST_ASSERT_FALSE(iot.pending());
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL_STRING("open", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("[sensor] open", iot.message());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: open"));
    TEST_ASSERT_EQUAL_STRING(
        "{\"api\":1,\"sensor\":{\"type\":\"contact\",\"status\":\"ok\",\"text\":\"open\","
        "\"active\":true},\"actuator\":null}", stateOf(iot).c_str());
    iot.ack();
    TEST_ASSERT_FALSE(iot.pending());

    iot.tick(IoTService::DEBOUNCE_MS + 10);        // same level: nothing new
    TEST_ASSERT_FALSE(iot.pending());

    // A bounce shorter than the debounce does not count
    mock::pinIn[2] = LOW;
    iot.tick(1000);
    mock::pinIn[2] = HIGH;
    iot.tick(1100);
    iot.tick(1100 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL_STRING("open", iot.sensorText());

    level(iot, 2, LOW, 2000);
    TEST_ASSERT_EQUAL_STRING("closed", iot.sensorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"active\":false"));
}

void test_custom_names_and_inverted_level_without_ntfy() {
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

// ── Actuator only: capabilities and commands ─────────────────────────────────

void test_on_off_obeys_commands() {
    actuator("relay", 0);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(logHas(logger, "IoT: actuator relay GPIO0"));
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[0]);
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);          // switches on low
    TEST_ASSERT_EQUAL_STRING("off", iot.actuatorText());
    TEST_ASSERT_EQUAL_STRING(L_A_RELAY, iot.actuatorDef()->label);   // panel text: I18n.h marker
    TEST_ASSERT_EQUAL_STRING(
        "{\"api\":1,\"sensor\":null,\"actuator\":{\"type\":\"relay\",\"capability\":\"onoff\","
        "\"on\":false,\"mode\":\"manual\",\"text\":\"off\"}}", stateOf(iot).c_str());

    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("on", 10));
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[0]);
    TEST_ASSERT_FALSE(iot.manual());                   // no rule, no mode
    TEST_ASSERT_EQUAL_STRING("[actuator] on", iot.message());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: [actuator] on"));

    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("toggle", 20));
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("toggle", 30));
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
    TEST_ASSERT_EQUAL(IoTService::UNSUPPORTED, iot.command("angle 9", 70));

    // Switches on high
    cfg.iot.actuatorHigh = true;
    iot.begin();
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[0]);
    iot.command("on", 0);
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);
}

void test_pulse_turns_off_by_itself_and_is_not_saved() {
    actuator("lock", 3);
    cfg.iot.restore = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(IoTService::OK, iot.command("pulse 3", 1000));
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count(IoTState::PATH));
    setMillis(1500);
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"pulse\":3"));
    setMillis(9000);                                   // past the end before the tick
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"pulse\":0"));

    iot.tick(3999);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.tick(4000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_FALSE(contains(stateOf(iot), "pulse"));
    IoTState st;
    TEST_ASSERT_TRUE(st.load());                       // saves the end of the pulse
    TEST_ASSERT_FALSE(st.on);

    // Another command cancels the pulse
    iot.command("pulse 3", 5000);
    iot.command("on", 5100);
    iot.tick(9000);
    TEST_ASSERT_TRUE(iot.actuatorOn());
}

void test_safety_shutoff() {
    actuator("pump", 0);
    cfg.iot.maxOn = 1;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    iot.command("on", 1000);
    iot.tick(1000 + 59999);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.tick(1000 + 60000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[actuator] off for safety", iot.message());
    TEST_ASSERT_TRUE(logHas(logger, "for safety"));

    // Off does not count time
    iot.tick(1000 + 200000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
}

void test_pwm_level() {
    actuator("fan", 0);
    cfg.iot.dutyOn = 80;
    cfg.iot.dutyOff = 20;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(20 * 255 / 100, mock::analogValue[0]);
    TEST_ASSERT_EQUAL_STRING("off", iot.actuatorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"capability\":\"level\",\"on\":false"));
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"level\":80"));

    iot.command("on", 0);
    TEST_ASSERT_EQUAL(80 * 255 / 100, mock::analogValue[0]);
    TEST_ASSERT_EQUAL_STRING("on 80%", iot.actuatorText());

    iot.command("35%", 0);
    TEST_ASSERT_EQUAL(35 * 255 / 100, mock::analogValue[0]);
    TEST_ASSERT_EQUAL(35, iot.level());

    iot.command("level 0", 0);                         // 0% = off
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("on", 0);                           // back to the on level
    TEST_ASSERT_EQUAL(80, iot.level());

    // Reconfiguring releases the output; GPIO0 goes back to the boot pull-up
    actuator("", -1);
    iot.begin();
    TEST_ASSERT_EQUAL(0, mock::analogValue[0]);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[0]);

    // Out-of-range values are clamped
    actuator("led", 1);
    cfg.iot.dutyOn = 250;
    cfg.iot.dutyOff = 250;
    iot.begin();
    TEST_ASSERT_EQUAL(255, mock::analogValue[1]);
    actuator("", -1);
    iot.begin();
    TEST_ASSERT_EQUAL(0, mock::analogValue[1]);
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[1]);       // TX is not a boot pin
}

void test_servo_position() {
    actuator("servo", 2);
    cfg.iot.angleOn = 150;
    cfg.iot.angleOff = 20;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(mock::servoAttached);
    TEST_ASSERT_EQUAL(2, mock::servoPin);
    TEST_ASSERT_EQUAL(20, mock::servoAngle);
    TEST_ASSERT_EQUAL_STRING("closed 20 deg", iot.actuatorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"angle\":20,\"ok\":true"));

    iot.command("open", 0);
    TEST_ASSERT_EQUAL(150, mock::servoAngle);
    TEST_ASSERT_EQUAL_STRING("open 150 deg", iot.actuatorText());
    iot.command("angle 90", 0);
    TEST_ASSERT_EQUAL(90, mock::servoAngle);
    TEST_ASSERT_EQUAL(90, iot.angle());
    iot.command("angle 20", 0);                       // the closed angle is off
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("on", 0);                           // on goes back to open
    TEST_ASSERT_EQUAL(150, mock::servoAngle);
    TEST_ASSERT_EQUAL(IoTService::UNSUPPORTED, iot.command("color #ffffff", 0));

    actuator("", -1);
    iot.begin();
    TEST_ASSERT_FALSE(mock::servoAttached);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);

    // A servo that does not attach: fails without writing
    mock::servoAttachResult = 0;
    mock::servoWrites = 0;
    actuator("servo", 3);
    iot.begin();
    TEST_ASSERT_TRUE(logHas(logger, "IoT: servo unavailable"));
    TEST_ASSERT_EQUAL_STRING("error", iot.actuatorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"ok\":false"));
    iot.command("open", 0);
    TEST_ASSERT_EQUAL(0, mock::servoWrites);
    actuator("", -1);
    iot.begin();
    TEST_ASSERT_EQUAL(255, mock::pinModeV[3]);         // RX is not a boot pin
}

void test_ws2812_strip_color() {
    actuator("strip", 2);
    cfg.iot.leds = 12;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[2]);
    TEST_ASSERT_EQUAL(2, mock::wsPin);
    TEST_ASSERT_EQUAL(12, mock::wsCount);
    TEST_ASSERT_EQUAL_HEX32(0, mock::wsColor);
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"color\":\"#ffffff\""));

    iot.command("color #ff8800", 0);
    TEST_ASSERT_EQUAL_HEX32(0xFF8800, mock::wsColor);
    TEST_ASSERT_EQUAL_STRING("on #ff8800 100%", iot.actuatorText());
    TEST_ASSERT_EQUAL_HEX32(0xFF8800, iot.color());

    iot.command("brightness 50", 0);
    TEST_ASSERT_EQUAL_HEX32(0x7F4400, mock::wsColor);
    iot.command("brightness 0", 0);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_HEX32(0, mock::wsColor);
    iot.command("color #0000ff", 0);                     // a color at brightness 0 lights at 100%
    TEST_ASSERT_EQUAL_HEX32(0x0000FF, mock::wsColor);
    iot.command("brightness 0", 0);
    iot.command("on", 0);                           // on too
    TEST_ASSERT_EQUAL(100, iot.level());

    // An out-of-range count becomes the maximum; releasing turns the strip off
    cfg.iot.leds = 0;
    iot.begin();
    TEST_ASSERT_EQUAL(IoTService::MAX_LEDS, mock::wsCount);
    cfg.iot.leds = 200;
    iot.begin();
    TEST_ASSERT_EQUAL(IoTService::MAX_LEDS, mock::wsCount);
    iot.command("on", 0);
    actuator("", -1);
    iot.begin();
    TEST_ASSERT_EQUAL_HEX32(0, mock::wsColor);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
}

// ── State at power-up ────────────────────────────────────────────────────────

void test_power_up_restores_last_command() {
    actuator("strip", 1);
    cfg.iot.restore = true;
    IoTService a{cfg, logger};
    a.begin();
    a.command("color #102030", 0);
    a.command("brightness 40", 0);

    IoTService b{cfg, logger};
    b.begin();
    TEST_ASSERT_TRUE(b.actuatorOn());
    TEST_ASSERT_EQUAL(40, b.level());
    TEST_ASSERT_EQUAL_HEX32(0x102030, b.color());

    // Out-of-range values in the file are clamped
    MockFS::put(IoTState::PATH, R"({"m":true,"on":true,"lv":150,"an":200,"c":4294967295})");
    actuator("servo", 2);
    IoTService c{cfg, logger};
    c.begin();
    TEST_ASSERT_EQUAL(180, c.angle());
    actuator("led", 0);
    IoTService d{cfg, logger};
    d.begin();
    TEST_ASSERT_EQUAL(100, d.level());

    // Broken or missing file: off
    MockFS::put(IoTState::PATH, "{");
    IoTService e{cfg, logger};
    e.begin();
    TEST_ASSERT_FALSE(e.actuatorOn());
    MockFS::reset();
    IoTService f{cfg, logger};
    f.begin();
    TEST_ASSERT_FALSE(f.actuatorOn());

    // Without the option, it neither saves nor reads
    cfg.iot.restore = false;
    IoTService g{cfg, logger};
    g.begin();
    g.command("on", 0);
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count(IoTState::PATH));

    // A failed save does not undo the command
    cfg.iot.restore = true;
    MockFS::failWriteOpen = true;
    IoTService h{cfg, logger};
    h.begin();
    TEST_ASSERT_EQUAL(IoTService::OK, h.command("on", 0));
}

// ── Sensor and actuator: rule ────────────────────────────────────────────────

void test_digital_rule_follows_and_manual_wins() {
    sensor("pir", 2);
    actuator("relay", 0);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.hasRule());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"mode\":\"auto\""));

    level(iot, 2, HIGH, 0);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[0]);
    TEST_ASSERT_EQUAL_STRING("[sensor] motion | on", iot.message());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: motion | on"));

    // Manual command: the rule does not touch it until "auto"
    iot.command("off", 500);
    TEST_ASSERT_TRUE(iot.manual());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"mode\":\"manual\""));
    level(iot, 2, LOW, 1000);
    level(iot, 2, HIGH, 2000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[sensor] motion | off", iot.message());

    iot.command("auto", 3000);                         // goes back and applies the rule
    TEST_ASSERT_FALSE(iot.manual());
    TEST_ASSERT_TRUE(iot.actuatorOn());

    // Pulse under a rule: when it ends, back to automatic
    level(iot, 2, LOW, 4000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("pulse 1", 5000);
    TEST_ASSERT_TRUE(iot.manual());
    iot.tick(6000);
    TEST_ASSERT_FALSE(iot.manual());
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // Safety under a rule: stays manual (the rule would turn it right back on)
    cfg.iot.maxOn = 1;
    iot.begin();
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.tick(60000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(iot.manual());
}

void test_auto_before_first_reading_and_rule_off() {
    sensor("button", 2);
    actuator("relay", 0);
    IoTService iot{cfg, logger};
    iot.begin();
    iot.command("on", 0);
    iot.command("auto", 0);                            // the rule has not decided yet
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.command("pulse 1", 0);
    iot.tick(1000);                                    // end of the pulse with no decision
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // Without the rule, the sensor only reports
    cfg.iot.rule = false;
    iot.begin();
    TEST_ASSERT_FALSE(iot.hasRule());
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"mode\":\"manual\""));
}

void test_digital_rule_toggles_on_each_activation() {
    sensor("button", 3);
    actuator("relay", 0);
    cfg.iot.toggle = true;
    IoTService iot{cfg, logger};
    iot.begin();
    level(iot, 3, HIGH, 0);                            // the 1st state only arms
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

void test_dht22_rule_with_margin() {
    sensor("dht22", 2);
    actuator("relay", 0);
    cfg.iot.above = true;
    cfg.iot.threshold = 40;
    cfg.iot.margin = 2;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());

    uint8_t d[5];
    frame(0x01, 0xE7, 0x01, 0x09, d);                  // 48.7% e 26.5 C
    feedDht(2, d);
    dhtRead(iot, 0);
    TEST_ASSERT_EQUAL_STRING("26.5 C, 48.7%", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[sensor] 26.5 C, 48.7% | on", iot.message());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"temperature\":26.5,\"humidity\":48.7"));
    uint8_t logged = logger.count();

    iot.tick(IoTService::READ_GAP_MS - 1);             // still within the interval
    feedDht(2, d);
    dhtRead(iot, IoTService::READ_GAP_MS);              // the same: not logged
    TEST_ASSERT_EQUAL(logged, logger.count());

    frame(0x01, 0x81, 0x01, 0x09, d);                  // 38.5%: inside the margin
    feedDht(2, d);
    dhtRead(iot, IoTService::READ_GAP_MS * 2);
    TEST_ASSERT_TRUE(iot.actuatorOn());

    frame(0x01, 0x72, 0x01, 0x09, d);                  // 37.0%: off
    feedDht(2, d);
    dhtRead(iot, IoTService::READ_GAP_MS * 3);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(logHas(logger, "| off"));
}

void test_dht_failures_negative_temperature_and_dht11() {
    sensor("dht22", 2);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();

    mock::bus = true;
    mock::busPin = 2;
    seg(HIGH, 5000);
    dhtRead(iot, 0);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
    TEST_ASSERT_TRUE(logHas(logger, "IoT: reading failed"));
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"status\":\"error\",\"text\":\"error\"}"));
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
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());

    uint8_t neg[5];
    frame(0x01, 0x5E, 0x80, 0x65, neg);                // 35.0% e -10.1 C
    feedDht(2, neg);
    dhtRead(iot, IoTService::READ_GAP_MS * 6);
    TEST_ASSERT_EQUAL_STRING("-10.1 C, 35.0%", iot.sensorText());
    TEST_ASSERT_EQUAL_STRING("[sensor] -10.1 C, 35.0%", iot.message());

    sensor("dht11", 2);
    actuator("relay", 0);
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
    TEST_ASSERT_TRUE(dht11.actuatorOn());               // temperature 26 > 25
    TEST_ASSERT_FALSE(dht11.pending());
}

// The DHT start pulse uses no delay: the pin stays LOW between two ticks.
// Before the minimum it does not read; after the maximum it restarts the pulse.
void test_dht_start_pulse_without_delay() {
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
        TEST_ASSERT_EQUAL(before, micros());          // did not wait at all
        TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[2]);
        TEST_ASSERT_EQUAL(LOW, mock::pinOut[2]);

        iot.tick(100 + c.wake - 1);                    // still in the pulse
        TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());

        iot.tick(100 + c.max + 1);                     // the loop ran late: restarts
        TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());
        TEST_ASSERT_EQUAL(LOW, mock::pinOut[2]);

        iot.tick(100 + c.max + 1 + c.wake);            // right window: reads
        TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
        TEST_ASSERT_TRUE(contains(std::string(iot.sensorText()), " C, "));
    }
}

void test_ds18b20_conversion_and_errors() {
    sensor("ds18b20", 2);
    actuator("relay", 0);
    cfg.iot.above = true;
    cfg.iot.threshold = 20;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();

    const uint8_t ok[] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x33};
    feedDs(2, true, true, ok, 9);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());
    iot.tick(IoTService::DS_CONVERT_MS - 1);
    TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());
    iot.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("25.0 C", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"temperature\":25.0}"));

    iot.tick(IoTService::READ_GAP_MS - 1);
    TEST_ASSERT_EQUAL_STRING("25.0 C", iot.sensorText());
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());

    IoTService ausente{cfg, logger};
    ausente.begin();
    feedDs(2, false, true, ok, 9);
    ausente.tick(0);
    TEST_ASSERT_EQUAL_STRING("error", ausente.sensorText());
    uint8_t logged = logger.count();
    ausente.tick(100);
    ausente.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL(logged, logger.count());

    IoTService reading{cfg, logger};
    reading.begin();
    feedDs(2, true, false, nullptr, 0);
    reading.tick(0);
    reading.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("error", reading.sensorText());

    uint8_t ruim[] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x00};
    IoTService crc{cfg, logger};
    crc.begin();
    feedDs(2, true, true, ruim, 9);
    crc.tick(0);
    crc.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL_STRING("error", crc.sensorText());

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
    TEST_ASSERT_TRUE(logHas(logger, "IoT: -10.5 C | on"));
}

void test_pulses_in_window_with_hysteresis() {
    sensor("pulses", 2);
    actuator("relay", 0);
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
    TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());
    IoTService::onPulse();
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_EQUAL_STRING("3 pulses/10 s", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"pulses\":3,\"total\":3}"));

    IoTService::onPulse();
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 2);         // 2 ≥ 3 - 1: stays on
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.tick(IoTService::PULSE_WINDOW_MS * 3);
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // Below the limit, with the margin upward
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

    // Changing the sensor releases the interrupt
    sensor("pir", 2);
    iot.begin();
    TEST_ASSERT_EQUAL(255, mock::isrPin);
}

// ── Time window ──────────────────────────────────────────────────────────────

// ── Units set on the IoT page: text, rule and ntfy use them; the API keeps
// °C, cm and pulses ──────────────────────────────────────────────────────

static const uint8_t DS_25C[] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x33};

static void dsRead(IoTService& iot, unsigned long t) {
    feedDs(2, true, true, DS_25C, 9);
    iot.tick(t);
    iot.tick(t + IoTService::DS_CONVERT_MS);
}

void test_units_temperature_in_fahrenheit() {
    sensor("ds18b20", 2);
    actuator("relay", 0);
    cfg.iot.unit = 1;
    cfg.iot.above = true;
    cfg.iot.threshold = 77;                            // °F: 25.0 °C is 77.0 °F
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    dsRead(iot, 0);
    TEST_ASSERT_EQUAL_STRING("77.0 F", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[sensor] 77.0 F | on", iot.message());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"temperature\":25.0}"));
    cfg.iot.threshold = 78;
    iot.begin();
    dsRead(iot, 0);
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // DHT watching the humidity: only the text changes unit
    sensor("dht22", 2);
    cfg.iot.metric = 0;
    cfg.iot.threshold = 40;
    iot.begin();
    uint8_t d[5];
    frame(0x01, 0xE7, 0x01, 0x09, d);                  // 48.7% and 26.5 C
    feedDht(2, d);
    dhtRead(iot, 0);
    TEST_ASSERT_EQUAL_STRING("79.7 F, 48.7%", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"temperature\":26.5,\"humidity\":48.7"));

    // Watching the temperature: the limit is in °F
    cfg.iot.metric = 1;
    cfg.iot.threshold = 80;
    iot.begin();
    feedDht(2, d);
    dhtRead(iot, 0);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    cfg.iot.threshold = 79;
    iot.begin();
    feedDht(2, d);
    dhtRead(iot, 0);
    TEST_ASSERT_TRUE(iot.actuatorOn());

    // A unit this version does not know reads as the first
    sensor("ds18b20", 2);
    cfg.iot.unit = 7;
    iot.begin();
    dsRead(iot, 0);
    TEST_ASSERT_EQUAL_STRING("25.0 C", iot.sensorText());
}

// A tank from the distance: level in % or volume in L, between the
// distances with the tank empty and full
void test_units_tank_level_and_volume() {
    sensor("distance", 2);
    actuator("pump", 0);
    cfg.iot.unit = 1;
    cfg.iot.tankEmpty = 110;
    cfg.iot.tankFull = 10;
    cfg.iot.threshold = 20;                            // on below 20 %
    IoTService iot{cfg, logger};
    iot.begin();
    echo(2, 60 * 58);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("50%", iot.sensorText());
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"distance\":60}"));
    echo(2, 95 * 58);
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL_STRING("15%", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    echo(2, 5 * 58);                                   // above the full mark
    iot.tick(IoTService::READ_GAP_MS * 2);
    TEST_ASSERT_EQUAL_STRING("100%", iot.sensorText());
    echo(2, 200 * 58);                                 // below the empty mark
    iot.tick(IoTService::READ_GAP_MS * 3);
    TEST_ASSERT_EQUAL_STRING("0%", iot.sensorText());

    cfg.iot.unit = 2;
    cfg.iot.tankL = 2000;
    cfg.iot.threshold = 500;                           // litres
    iot.begin();
    echo(2, 60 * 58);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("1000 L", iot.sensorText());
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // A tank with no height (a hand-edited file) reads 0
    cfg.iot.tankFull = 110;
    iot.begin();
    echo(2, 60 * 58);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("0 L", iot.sensorText());
}

// A flow meter: L/min (tenths) or mL/min, and the pulses since boot
void test_units_flow_and_total() {
    sensor("pulses", 2);
    actuator("valve", 0);
    cfg.iot.unit = 1;
    cfg.iot.ppl = 450;
    cfg.iot.above = true;
    cfg.iot.threshold = 1;                             // on from 1.0 L/min
    IoTService iot{cfg, logger};
    iot.begin();
    iot.tick(0);
    for (int i = 0; i < 75; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_EQUAL_STRING("1.0 L/min", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"pulses\":75,\"total\":75}"));
    for (int i = 0; i < 45; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS * 2);
    TEST_ASSERT_EQUAL_STRING("0.6 L/min", iot.sensorText());
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"pulses\":45,\"total\":120}"));

    cfg.iot.unit = 2;
    iot.begin();
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"status\":\"waiting\""));
    iot.tick(0);
    for (int i = 0; i < 75; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_EQUAL_STRING("1000 mL/min", iot.sensorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"total\":75}"));     // a new setup starts over

    // A flow beyond what the text holds stops at 3276.7 L/min
    cfg.iot.unit = 1;
    cfg.iot.ppl = 1;
    iot.begin();
    iot.tick(0);
    for (int i = 0; i < 600; i++) IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_EQUAL_STRING("3276.7 L/min", iot.sensorText());

    // No pulses per litre (a hand-edited file): 0
    cfg.iot.ppl = 0;
    iot.begin();
    iot.tick(0);
    IoTService::onPulse();
    iot.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_EQUAL_STRING("0.0 L/min", iot.sensorText());
}

static time_t clockNow = 0;
static time_t fakeClock() { return clockNow; }
static const time_t DAY0 = 1704067200;   // 01/01/2024 00:00 UTC
static void at(int h, int m) { clockNow = DAY0 + h * 3600 + m * 60; }

void test_local_time_and_window() {
    uint16_t m = 0;
    IoTService none{cfg, logger};
    TEST_ASSERT_FALSE(none.localMinute(m));

    Clock clock{fakeClock};
    IoTService iot{cfg, logger, &clock};
    clockNow = 1000;                                   // 1970
    TEST_ASSERT_FALSE(iot.localMinute(m));
    TEST_ASSERT_TRUE(iot.inHours());                   // window off

    at(2, 30);
    cfg.tz = -180;                                     // goes back to the previous day
    TEST_ASSERT_TRUE(iot.localMinute(m));
    TEST_ASSERT_EQUAL(23 * 60 + 30, m);
    cfg.tz = 330;
    TEST_ASSERT_TRUE(iot.localMinute(m));
    TEST_ASSERT_EQUAL(8 * 60, m);
    at(22, 0);
    cfg.tz = 14 * 60;                                  // goes on to the next day
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
    cfg.iot.winStart = 22 * 60;                        // wraps past midnight
    cfg.iot.winEnd = 6 * 60;
    iot.begin();
    struct { int h, m; bool in; } night[] = {{21, 59, false}, {22, 0, true}, {23, 0, true},
                                             {5, 59, true}, {6, 0, false}};
    for (auto& c : night) {
        at(c.h, c.m);
        TEST_ASSERT_EQUAL(c.in, iot.inHours());
    }
    clockNow = 1000;
    TEST_ASSERT_FALSE(iot.inHours());                  // clock in 1970: outside
}

void test_rule_only_inside_window() {
    sensor("pir", 2);
    actuator("relay", 0);
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
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"hours\":true"));

    at(18, 0);
    iot.tick(1000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_FALSE(iot.manual());
    TEST_ASSERT_EQUAL_STRING("[actuator] off (window closed)", iot.message());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"mode\":\"auto\",\"text\":\"off\",\"hours\":false"));
    level(iot, 2, LOW, 2000);
    level(iot, 2, HIGH, 3000);                         // the rule asks, the window does not allow it
    TEST_ASSERT_FALSE(iot.actuatorOn());

    // A manual command holds outside the window; "auto" turns it off again
    iot.command("on", 4000);
    iot.tick(5000);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.command("auto", 6000);
    TEST_ASSERT_FALSE(iot.actuatorOn());

    at(8, 0);
    iot.tick(7000);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[actuator] on (window opened)", iot.message());

    // In manual mode, the window does not touch it
    iot.command("off", 8000);
    at(19, 0);
    iot.tick(9000);
    at(9, 0);
    iot.tick(10000);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_TRUE(iot.manual());

    // Without the window, the JSON has no "hours"
    cfg.iot.window = false;
    iot.begin();
    TEST_ASSERT_FALSE(contains(stateOf(iot), "hours"));

    // Actuator only: the window has no rule to limit
    cfg.iot.window = true;
    cfg.iot.sensorPin = -1;
    iot.begin();
    iot.command("on", 0);
    at(20, 0);
    iot.tick(100);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_FALSE(contains(stateOf(iot), "hours"));
}

// Outside the window, "auto" and the end of a pulse turn it off even without a reading
void test_outside_window_without_reading() {
    sensor("button", 2);
    actuator("relay", 0);
    cfg.iot.window = true;
    cfg.iot.winStart = 8 * 60;
    cfg.iot.winEnd = 9 * 60;
    Clock clock{fakeClock};
    at(20, 0);
    IoTService iot{cfg, logger, &clock};
    iot.begin();
    iot.tick(0);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("on", 0);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    iot.command("auto", 10);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    iot.command("pulse 1", 20);
    iot.tick(1020);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_FALSE(iot.manual());
}

// Proportional level: outside the window, the off level
void test_proportional_outside_window() {
    sensor("pulses", 2);
    actuator("fan", 0);
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
    TEST_ASSERT_EQUAL(20 * 255 / 100, mock::analogValue[0]);
    at(9, 0);
    iot.tick(IoTService::PULSE_WINDOW_MS + 2);
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(60, iot.level());
}

// ── SHT3x (I2C) ──────────────────────────────────────────────────────────────

static uint8_t shtCrc(uint8_t a, uint8_t b) {
    uint8_t crc = 0xFF;
    for (uint8_t v : {a, b}) {
        crc ^= v;
        for (int i = 0; i < 8; i++) crc = (uint8_t)(crc & 0x80 ? (crc << 1) ^ 0x31 : crc << 1);
    }
    return crc;
}

static std::vector<uint8_t> shtFrame(uint16_t t, uint16_t h) {
    uint8_t t1 = (uint8_t)(t >> 8), t0 = (uint8_t)t, h1 = (uint8_t)(h >> 8), h0 = (uint8_t)h;
    return {t1, t0, shtCrc(t1, t0), h1, h0, shtCrc(h1, h0)};
}

void test_sht3x_reads_temperature_and_humidity() {
    TEST_ASSERT_EQUAL_HEX8(0x92, shtCrc(0xBE, 0xEF));    // datasheet example
    sensor("sht3x", 0);                                   // SDA on GPIO0, SCL on GPIO2
    actuator("relay", 2);                                  // on SCL: left out
    I2cSim sim(0, 2);
    sim.present = {0x44};
    std::vector<uint8_t> frame = shtFrame(0x6666, 0x8000);   // 25.0 C and 50.0 %
    sim.reply = [&](uint8_t, const std::vector<uint8_t>&) { return frame; };
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.hasSensor());
    TEST_ASSERT_FALSE(iot.hasActuator());
    TEST_ASSERT_TRUE(iot.sensorUses(0));
    TEST_ASSERT_TRUE(iot.sensorUses(2));
    TEST_ASSERT_FALSE(iot.sensorUses(1));
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);

    iot.tick(0);                                          // asks for a measurement
    TEST_ASSERT_TRUE(sim.writes.back() == std::vector<uint8_t>({0x44, 0x24, 0x00}));
    iot.tick(IoTService::SHT_MEASURE_MS - 1);
    TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());
    iot.tick(IoTService::SHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("25.0 C, 50.0%", iot.sensorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"temperature\":25.0,\"humidity\":50.0"));
    size_t asked = sim.writes.size();
    iot.tick(IoTService::READ_GAP_MS - 1);                // one measurement every READ_GAP_MS
    TEST_ASSERT_EQUAL(asked, sim.writes.size());

    // Below zero and at the bottom of the scale
    frame = shtFrame(0x0000, 0xFFFF);
    iot.tick(IoTService::READ_GAP_MS);
    iot.tick(IoTService::READ_GAP_MS + IoTService::SHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("-45.0 C, 100.0%", iot.sensorText());

    // Wrong CRC in each half and a sensor that does not answer
    frame = shtFrame(0x6666, 0x8000);
    frame[2] ^= 1;
    unsigned long t = 2 * IoTService::READ_GAP_MS;
    iot.tick(t);
    iot.tick(t + IoTService::SHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
    frame = shtFrame(0x6666, 0x8000);
    frame[5] ^= 1;
    t += IoTService::READ_GAP_MS;
    iot.tick(t);
    iot.tick(t + IoTService::SHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
    sim.present.clear();
    t += IoTService::READ_GAP_MS;
    iot.tick(t);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
    sim.present = {0x44};
    frame = shtFrame(0x6666, 0x8000);
    t += IoTService::READ_GAP_MS;
    iot.tick(t);                                          // asks again
    sim.present.clear();
    iot.tick(t + IoTService::SHT_MEASURE_MS);             // gone before the read
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
}

// The rule uses the chosen value, as with the DHT
// AHT20 (0x38): a measurement command, then status and 20-bit values;
// a sensor not calibrated yet gets its init command
static std::vector<uint8_t> ahtFrame(uint8_t status, uint32_t h, uint32_t t) {
    return {status, (uint8_t)(h >> 12), (uint8_t)(h >> 4), (uint8_t)((h & 0x0F) << 4 | t >> 16),
            (uint8_t)(t >> 8), (uint8_t)t};
}

void test_aht20_reads_temperature_and_humidity() {
    sensor("aht20", 0);                                   // SDA on GPIO0, SCL on GPIO2
    actuator("relay", 3);
    cfg.iot.metric = 1;
    cfg.iot.above = true;
    cfg.iot.threshold = 24;
    I2cSim sim(0, 2);
    sim.present = {0x38};
    std::vector<uint8_t> frame = ahtFrame(0x18, 0x80000, 0x60000);   // 50.0 % and 25.0 C
    sim.reply = [&](uint8_t, const std::vector<uint8_t>&) { return frame; };
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.sensorUses(0));
    TEST_ASSERT_TRUE(iot.sensorUses(2));
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
    iot.tick(0);
    TEST_ASSERT_TRUE(sim.writes.back() == std::vector<uint8_t>({0x38, 0xAC, 0x33, 0x00}));
    iot.tick(IoTService::AHT_MEASURE_MS - 1);
    TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());
    iot.tick(IoTService::AHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("25.0 C, 50.0%", iot.sensorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"temperature\":25.0,\"humidity\":50.0"));
    TEST_ASSERT_TRUE(iot.actuatorOn());                   // 25.0 C above 24

    // The ends of the scale
    frame = ahtFrame(0x18, 0, 0);
    unsigned long t = IoTService::READ_GAP_MS;
    iot.tick(t);
    iot.tick(t + IoTService::AHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("-50.0 C, 0.0%", iot.sensorText());

    // Not calibrated: the init command, and the reading waits for the next turn
    frame = ahtFrame(0x10, 0x80000, 0x60000);
    t += IoTService::READ_GAP_MS;
    iot.tick(t);
    iot.tick(t + IoTService::AHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
    TEST_ASSERT_TRUE(sim.writes.back() == std::vector<uint8_t>({0x38, 0xBE, 0x08, 0x00}));
    // Still busy: no value either
    frame = ahtFrame(0x98, 0x80000, 0x60000);
    t += IoTService::READ_GAP_MS;
    iot.tick(t);
    iot.tick(t + IoTService::AHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
    // Back, in °F
    cfg.iot.unit = 1;
    iot.begin();
    frame = ahtFrame(0x18, 0x80000, 0x60000);
    iot.tick(0);
    iot.tick(IoTService::AHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("77.0 F, 50.0%", iot.sensorText());
    // Gone between the command and the reading, and gone altogether
    t = IoTService::READ_GAP_MS;
    iot.tick(t);
    sim.present.clear();
    iot.tick(t + IoTService::AHT_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
    iot.tick(t + IoTService::READ_GAP_MS);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
}

// BH1750 (0x23): one high-resolution reading at a time, in lux
void test_bh1750_reads_light() {
    sensor("bh1750", 3);                                  // SDA on RX, SCL on TX
    actuator("relay", 0);
    cfg.iot.threshold = 100;                              // on below 100 lx
    cfg.iot.notify = true;
    I2cSim sim(3, 1);
    sim.present = {0x23};
    std::vector<uint8_t> frame = {0x01, 0x2C};            // 300 counts: 250 lx
    sim.reply = [&](uint8_t, const std::vector<uint8_t>&) { return frame; };
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.sensorUses(1));
    iot.tick(0);
    TEST_ASSERT_TRUE(sim.writes.back() == std::vector<uint8_t>({0x23, 0x20}));
    iot.tick(IoTService::LUX_MEASURE_MS - 1);
    TEST_ASSERT_EQUAL_STRING("waiting", iot.sensorText());
    iot.tick(IoTService::LUX_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("250 lx", iot.sensorText());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"lux\":250}"));
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("[sensor] 250 lx | off", iot.message());

    // Dark: the rule turns it on; the top of the scale
    frame = {0x00, 0x30};                                 // 48 counts: 40 lx
    unsigned long t = IoTService::READ_GAP_MS;
    iot.tick(t);
    iot.tick(t + IoTService::LUX_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("40 lx", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    frame = {0xFF, 0xFF};
    t += IoTService::READ_GAP_MS;
    iot.tick(t);
    iot.tick(t + IoTService::LUX_MEASURE_MS);
    TEST_ASSERT_EQUAL_STRING("54612 lx", iot.sensorText());

    // No answer
    sim.present.clear();
    t += IoTService::READ_GAP_MS;
    iot.tick(t);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());
}

void test_sht3x_drives_by_temperature() {
    sensor("sht3x", 3);                                   // SDA on RX, SCL on TX
    actuator("fan", 0);
    cfg.iot.metric = 1;
    cfg.iot.above = true;
    cfg.iot.threshold = 24;
    I2cSim sim(3, 1);
    sim.present = {0x44};
    sim.reply = [](uint8_t, const std::vector<uint8_t>&) { return shtFrame(0x6666, 0x1000); };
    IoTService iot{cfg, logger};
    iot.begin();
    TEST_ASSERT_TRUE(iot.hasActuator());
    iot.tick(0);
    iot.tick(IoTService::SHT_MEASURE_MS);
    TEST_ASSERT_TRUE(iot.actuatorOn());                   // 25.0 C above 24
}

void test_ultrasonic_distance_and_failures() {
    sensor("distance", 2);
    actuator("relay", 0);
    cfg.iot.threshold = 10;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();

    echo(2, 580);
    iot.tick(0);
    TEST_ASSERT_EQUAL_STRING("10 cm", iot.sensorText());
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_TRUE(contains(stateOf(iot), "\"distance\":10}"));

    echo(2, 0);
    iot.tick(IoTService::READ_GAP_MS - 1);
    TEST_ASSERT_EQUAL_STRING("10 cm", iot.sensorText());

    echo(2, 15 * 58);
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_FALSE(iot.actuatorOn());

    echo(2, 2);
    iot.tick(IoTService::READ_GAP_MS * 2);
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());

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
    TEST_ASSERT_EQUAL_STRING("error", iot.sensorText());

    echo(2, 400 * 58);
    iot.tick(IoTService::READ_GAP_MS * 6);
    TEST_ASSERT_EQUAL_STRING("400 cm", iot.sensorText());
}

void test_proportional_level() {
    sensor("ds18b20", 2);
    actuator("fan", 0);
    cfg.iot.linear = true;
    cfg.iot.threshold = 20;
    cfg.iot.thresholdHi = 30;
    cfg.iot.dutyOn = 100;
    cfg.iot.dutyOff = 0;
    IoTService iot{cfg, logger};
    iot.begin();

    struct { uint8_t lo, hi, crc; int duty; } reads[] = {
        {0x40, 0x01, 0, 0},     // 20.0 C: at the limit, off
        {0x90, 0x01, 0, 50},    // 25.0 C: halfway
        {0xF0, 0x01, 0, 100},   // 31.0 C: above the high limit
        {0x90, 0x01, 0, 50},
        {0x90, 0x01, 0, 50},    // the same: nothing changes
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
        TEST_ASSERT_EQUAL(r.duty * 255 / 100, mock::analogValue[0]);
        t += IoTService::READ_GAP_MS;
    }
    TEST_ASSERT_TRUE(iot.actuatorOn());
    TEST_ASSERT_EQUAL(50, iot.level());

    // Empty range: two steps
    cfg.iot.thresholdHi = 20;
    iot.begin();
    uint8_t b25[] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x33};
    feedDs(2, true, true, b25, 9);
    iot.tick(0);
    iot.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL(255, mock::analogValue[0]);
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

    // A plain rule with a level: on uses the on level
    cfg.iot.linear = false;
    cfg.iot.above = true;
    cfg.iot.dutyOn = 60;
    iot.begin();
    iot.command("level 10", 0);
    iot.command("off", 0);
    iot.command("auto", 0);
    feedDs(2, true, true, b25, 9);
    iot.tick(0);
    iot.tick(IoTService::DS_CONVERT_MS);
    TEST_ASSERT_EQUAL(60, iot.level());

    // Proportional does not apply to a digital sensor
    sensor("pir", 2);
    cfg.iot.linear = true;
    iot.begin();
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_EQUAL(60 * 255 / 100, mock::analogValue[0]);
}

// Distance and pulses have no tenths; proportional asked for a relay is ignored
void test_proportional_without_tenths_and_numeric_manual() {
    sensor("distance", 2);
    actuator("led", 0);
    cfg.iot.linear = true;
    cfg.iot.threshold = 10;
    cfg.iot.thresholdHi = 30;
    IoTService iot{cfg, logger};
    iot.begin();
    echo(2, 20 * 58);
    iot.tick(0);
    TEST_ASSERT_EQUAL(50, iot.level());

    // Manual: the reading changes, the output does not
    iot.command("off", 0);
    echo(2, 30 * 58);
    iot.tick(IoTService::READ_GAP_MS);
    TEST_ASSERT_FALSE(iot.actuatorOn());
    TEST_ASSERT_EQUAL_STRING("30 cm", iot.sensorText());

    actuator("relay", 0);
    cfg.iot.above = true;
    iot.begin();
    echo(2, 20 * 58);
    iot.tick(0);
    TEST_ASSERT_TRUE(iot.actuatorOn());                // 20 ≥ 10: plain rule
}

void test_rule_opens_the_servo() {
    sensor("pir", 2);
    actuator("servo", 0);
    cfg.iot.angleOn = 120;
    IoTService iot{cfg, logger};
    iot.begin();
    iot.command("angle 45", 0);
    iot.command("close", 0);
    iot.command("auto", 0);
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_EQUAL(120, mock::servoAngle);
}

// An urgent notice stays urgent until it goes out, even with readings in between
void test_urgent_stays_until_sent() {
    sensor("pir", 2);
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    iot.begin();
    level(iot, 2, HIGH, 0);
    TEST_ASSERT_TRUE(iot.urgent());
    level(iot, 2, LOW, 1000);
    TEST_ASSERT_TRUE(iot.urgent());
    TEST_ASSERT_EQUAL_STRING("[sensor] clear", iot.message());
    iot.ack();
    TEST_ASSERT_FALSE(iot.urgent());
    TEST_ASSERT_FALSE(iot.pending());
}

// A field of the wrong type or out of range keeps its default; an unknown one is ignored
void test_state_file_with_wrong_types() {
    MockFS::put(IoTState::PATH, R"({"m":1,"on":"yes","lv":-1,"an":256,"c":-5,"x":true})");
    IoTState st;
    TEST_ASSERT_TRUE(st.load());
    TEST_ASSERT_FALSE(st.manual);
    TEST_ASSERT_FALSE(st.on);
    TEST_ASSERT_EQUAL(100, st.level);
    TEST_ASSERT_EQUAL(90, st.angle);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, st.rgb);
}

// The largest saved state fits in the read text; a larger file is ignored
void test_state_at_the_text_limit() {
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

    // Larger than the text: cut, invalid JSON, defaults
    std::string big = R"({"m":true,"on":true,"lv":40,"an":10,"c":1,"x":")";
    big += std::string(IoTState::MAX_TEXT, 'x') + "\"}";
    MockFS::put(IoTState::PATH, big);
    IoTState cut;
    TEST_ASSERT_FALSE(cut.load());
    TEST_ASSERT_FALSE(cut.on);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_catalog_by_identifier);
    RUN_TEST(test_pins_and_parse);
    RUN_TEST(test_text_commands);
    RUN_TEST(test_off_leaves_pins_alone);
    RUN_TEST(test_json_in_short_buffer);
    RUN_TEST(test_unknown_type_turns_part_off);
    RUN_TEST(test_same_pin_stays_with_sensor);
    RUN_TEST(test_sensor_only_publishes_with_type_names);
    RUN_TEST(test_custom_names_and_inverted_level_without_ntfy);
    RUN_TEST(test_on_off_obeys_commands);
    RUN_TEST(test_pulse_turns_off_by_itself_and_is_not_saved);
    RUN_TEST(test_safety_shutoff);
    RUN_TEST(test_pwm_level);
    RUN_TEST(test_servo_position);
    RUN_TEST(test_ws2812_strip_color);
    RUN_TEST(test_power_up_restores_last_command);
    RUN_TEST(test_digital_rule_follows_and_manual_wins);
    RUN_TEST(test_auto_before_first_reading_and_rule_off);
    RUN_TEST(test_digital_rule_toggles_on_each_activation);
    RUN_TEST(test_dht22_rule_with_margin);
    RUN_TEST(test_dht_failures_negative_temperature_and_dht11);
    RUN_TEST(test_dht_start_pulse_without_delay);
    RUN_TEST(test_ds18b20_conversion_and_errors);
    RUN_TEST(test_pulses_in_window_with_hysteresis);
    RUN_TEST(test_units_temperature_in_fahrenheit);
    RUN_TEST(test_units_tank_level_and_volume);
    RUN_TEST(test_units_flow_and_total);
    RUN_TEST(test_local_time_and_window);
    RUN_TEST(test_rule_only_inside_window);
    RUN_TEST(test_outside_window_without_reading);
    RUN_TEST(test_proportional_outside_window);
    RUN_TEST(test_sht3x_reads_temperature_and_humidity);
    RUN_TEST(test_sht3x_drives_by_temperature);
    RUN_TEST(test_aht20_reads_temperature_and_humidity);
    RUN_TEST(test_bh1750_reads_light);
    RUN_TEST(test_ultrasonic_distance_and_failures);
    RUN_TEST(test_proportional_level);
    RUN_TEST(test_proportional_without_tenths_and_numeric_manual);
    RUN_TEST(test_rule_opens_the_servo);
    RUN_TEST(test_urgent_stays_until_sent);
    RUN_TEST(test_state_file_with_wrong_types);
    RUN_TEST(test_state_at_the_text_limit);
    return UNITY_END();
}
