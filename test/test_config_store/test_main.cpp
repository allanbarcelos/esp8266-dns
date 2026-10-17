// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <cstring>
#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "ConfigStore.h"
#include "Totp.h"

static Logger      logger;
static Config      config;
static ConfigStore store(config, logger);

void setUp() {
    resetMocks();
    logger = Logger();
    config = Config{};
}
void tearDown() {}

void test_begin_ok() {
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_TRUE(logHas(logger, "LittleFS ready"));
}

void test_begin_fails() {
    MockFS::beginOk = false;
    TEST_ASSERT_FALSE(store.begin());
    TEST_ASSERT_TRUE(logHas(logger, "LittleFS mount failed"));
}

void test_load_false_without_file() {
    TEST_ASSERT_FALSE(store.load());
}

void test_load_open_fails() {
    MockFS::put("/config.json", "{}");
    MockFS::failReadOpen = true;
    TEST_ASSERT_FALSE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "Could not open"));
}

void test_load_false_on_invalid_json() {
    MockFS::put("/config.json", "{ broken json !!!");
    TEST_ASSERT_FALSE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "JSON error"));
}

// A file larger than anything save() writes: not even read
void test_load_file_too_large() {
    MockFS::put("/config.json", std::string(ConfigStore::MAX_FILE, ' '));
    TEST_ASSERT_FALSE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "config.json unreadable (4096 bytes)"));
}

// No memory for the text: a clean failure, without touching the configuration
void test_load_without_memory() {
    MockFS::put("/config.json", R"({"ssid":"home"})");
    mock::failMallocSize = std::strlen(R"({"ssid":"home"})") + 1;
    TEST_ASSERT_FALSE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "config.json unreadable (15 bytes)"));
    TEST_ASSERT_EQUAL_STRING("", config.ssid.c_str());
}

// Just below the limit it is still read
void test_load_at_the_size_limit() {
    std::string json = R"({"ssid":"home"})";
    json += std::string(ConfigStore::MAX_FILE - 1 - json.size(), ' ');
    MockFS::put("/config.json", json);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("home", config.ssid.c_str());
}

void test_load_defaults() {
    MockFS::put("/config.json", R"({"v":2,"ssid":"net"})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("admin", config.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.webpss.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.pass.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.cf_record);
    TEST_ASSERT_TRUE(config.ota_auto);
    TEST_ASSERT_EQUAL(5, config.ntfy_poll);
    const IoTConfig& t = config.iot;
    TEST_ASSERT_EQUAL_STRING("", t.sensor);
    TEST_ASSERT_EQUAL(-1, t.sensorPin);
    TEST_ASSERT_TRUE(t.sensorHigh);
    TEST_ASSERT_EQUAL_STRING("", t.actuator);
    TEST_ASSERT_EQUAL(-1, t.actuatorPin);
    TEST_ASSERT_FALSE(t.actuatorHigh);
    TEST_ASSERT_EQUAL(90, t.angleOn);
    TEST_ASSERT_EQUAL(0, t.angleOff);
    TEST_ASSERT_EQUAL(100, t.dutyOn);
    TEST_ASSERT_EQUAL(0, t.dutyOff);
    TEST_ASSERT_EQUAL(8, t.leds);
    TEST_ASSERT_EQUAL(0, t.maxOn);
    TEST_ASSERT_FALSE(t.restore);
    TEST_ASSERT_TRUE(t.rule);
    TEST_ASSERT_FALSE(t.notify);
    // Already at the current version: nothing is rewritten
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/config.v1.json"));
}

// v2 format: every field comes back the same
void test_iot_v2_round_trip() {
    IoTConfig& t = config.iot;
    strlcpy(t.sensor, "dht22", sizeof(t.sensor));
    t.sensorPin = 2;
    t.sensorHigh = false;
    strlcpy(t.onText, "molhado", sizeof(t.onText));
    strlcpy(t.offText, "seco", sizeof(t.offText));
    t.metric = 1;
    t.notify = true;
    strlcpy(t.actuator, "fan", sizeof(t.actuator));
    t.actuatorPin = 0;
    t.actuatorHigh = true;
    t.angleOn = 170;
    t.angleOff = 10;
    t.dutyOn = 80;
    t.dutyOff = 20;
    t.leds = 30;
    t.maxOn = 90;
    t.restore = true;
    t.rule = false;
    t.above = true;
    t.threshold = -10;
    t.margin = 2;
    t.toggle = true;
    t.linear = true;
    t.thresholdHi = 40;
    TEST_ASSERT_TRUE(store.save());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"v\":3"));

    config = Config{};
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "Configuration loaded"));
    TEST_ASSERT_EQUAL_STRING("dht22", t.sensor);
    TEST_ASSERT_EQUAL(2, t.sensorPin);
    TEST_ASSERT_FALSE(t.sensorHigh);
    TEST_ASSERT_EQUAL_STRING("molhado", t.onText);
    TEST_ASSERT_EQUAL_STRING("seco", t.offText);
    TEST_ASSERT_EQUAL(1, t.metric);
    TEST_ASSERT_TRUE(t.notify);
    TEST_ASSERT_EQUAL_STRING("fan", t.actuator);
    TEST_ASSERT_EQUAL(0, t.actuatorPin);
    TEST_ASSERT_TRUE(t.actuatorHigh);
    TEST_ASSERT_EQUAL(170, t.angleOn);
    TEST_ASSERT_EQUAL(10, t.angleOff);
    TEST_ASSERT_EQUAL(80, t.dutyOn);
    TEST_ASSERT_EQUAL(20, t.dutyOff);
    TEST_ASSERT_EQUAL(30, t.leds);
    TEST_ASSERT_EQUAL(90, t.maxOn);
    TEST_ASSERT_TRUE(t.restore);
    TEST_ASSERT_FALSE(t.rule);
    TEST_ASSERT_TRUE(t.above);
    TEST_ASSERT_EQUAL(-10, t.threshold);
    TEST_ASSERT_EQUAL(2, t.margin);
    TEST_ASSERT_TRUE(t.toggle);
    TEST_ASSERT_TRUE(t.linear);
    TEST_ASSERT_EQUAL(40, t.thresholdHi);
}

// Configuration from before the identifier catalog (no "v")
static const char* V1 = R"({"ssid":"net","iot_sensor":2,"iot_module":0,
    "iot_sensor_type":2,"iot_module_type":6,"iot_sensor_high":false,
    "iot_module_high":true,"iot_notify":true,"iot_metric":1,"iot_above":true,
    "iot_threshold":30,"iot_margin":2,"iot_angle_on":120,"iot_angle_off":5,
    "iot_duty_on":70,"iot_duty_off":10,"iot_manual":1,"iot_toggle":true,
    "iot_linear":true,"iot_threshold_hi":40})";

void test_v1_migration_full() {
    MockFS::put("/config.json", V1);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "migrated to v3"));
    const IoTConfig& t = config.iot;
    TEST_ASSERT_EQUAL_STRING("dht22", t.sensor);
    TEST_ASSERT_EQUAL(2, t.sensorPin);
    TEST_ASSERT_FALSE(t.sensorHigh);
    TEST_ASSERT_EQUAL_STRING("fan", t.actuator);
    TEST_ASSERT_EQUAL(0, t.actuatorPin);
    TEST_ASSERT_TRUE(t.actuatorHigh);
    TEST_ASSERT_TRUE(t.notify);
    TEST_ASSERT_EQUAL(1, t.metric);
    TEST_ASSERT_TRUE(t.above);
    TEST_ASSERT_EQUAL(30, t.threshold);
    TEST_ASSERT_EQUAL(2, t.margin);
    TEST_ASSERT_EQUAL(120, t.angleOn);
    TEST_ASSERT_EQUAL(5, t.angleOff);
    TEST_ASSERT_EQUAL(70, t.dutyOn);
    TEST_ASSERT_EQUAL(10, t.dutyOff);
    TEST_ASSERT_TRUE(t.toggle);
    TEST_ASSERT_TRUE(t.linear);
    TEST_ASSERT_EQUAL(40, t.thresholdHi);
    TEST_ASSERT_TRUE(t.rule);

    // "Command: on" becomes a saved manual state, restored at boot
    TEST_ASSERT_TRUE(t.restore);
    IoTState st;
    TEST_ASSERT_TRUE(st.load());
    TEST_ASSERT_TRUE(st.manual);
    TEST_ASSERT_TRUE(st.on);
    TEST_ASSERT_EQUAL(70, st.level);
    TEST_ASSERT_EQUAL(120, st.angle);

    // Original kept; the new file is already the current version
    TEST_ASSERT_EQUAL_STRING(V1, MockFS::files["/config.v1.json"].c_str());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"v\":3"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"act\":\"fan\""));
    TEST_ASSERT_EQUAL_STRING("net", config.ssid.c_str());
}

void test_v1_migration_forced_off_and_types_out_of_table() {
    MockFS::put("/config.json", R"({"iot_sensor":3,"iot_module":1,
        "iot_sensor_type":99,"iot_module_type":99,"iot_manual":2})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("digital", config.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("relay", config.iot.actuator);
    IoTState st;
    TEST_ASSERT_TRUE(st.load());
    TEST_ASSERT_TRUE(st.manual);
    TEST_ASSERT_FALSE(st.on);
}

void test_v1_migration_without_sensor_keeps_module_idle() {
    // v1 only drove the module with a sensor: without one, nothing starts driving
    MockFS::put("/config.json", R"({"iot_sensor":-1,"iot_module":0,"iot_manual":1})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("", config.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("", config.iot.actuator);
    TEST_ASSERT_EQUAL(-1, config.iot.actuatorPin);
    TEST_ASSERT_FALSE(config.iot.restore);
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/iot.state"));

    // Same pin for the sensor and the module: only the sensor
    MockFS::reset();
    MockFS::put("/config.json", R"({"iot_sensor":2,"iot_module":2})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("digital", config.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("", config.iot.actuator);

    // Only the sensor
    MockFS::reset();
    MockFS::put("/config.json", R"({"iot_sensor":2})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("digital", config.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("", config.iot.actuator);

    // No fixed command, no saved state
    MockFS::reset();
    MockFS::put("/config.json", R"({"iot_sensor":2,"iot_module":0})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("relay", config.iot.actuator);
    TEST_ASSERT_FALSE(config.iot.restore);
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/iot.state"));
}

void test_migration_that_cannot_save_keeps_the_config() {
    MockFS::put("/config.json", V1);
    MockFS::failWriteOpen = true;
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("dht22", config.iot.sensor);
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/config.v1.json"));
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/iot.state"));
    TEST_ASSERT_TRUE(logHas(logger, "Could not save"));
    TEST_ASSERT_EQUAL_STRING(V1, MockFS::files["/config.json"].c_str());
}

// A version 2 file (the names in full): every field is read, by the hash of
// its name, and the next save writes the same values under the three letters
void test_v2_names_are_read() {
    MockFS::put("/config.json", "{\"v\":2,"
        R"("webusr":"usr0","webpss":"pbkdf2:32df4bf98374fb9500b6e2d939aefb9c:a09a1183612299e514bbc7bdcc881f941231a72c","totp":"00112233445566778899aabbccddeeff00112233","totp_step":4,)"
        R"("totp_bstep":5,"ssid":"sid5","pass":"wpw6","ntfy_topic":"ntt7",)"
        R"("ntfy_cmd":"ntc8","ntfy_token":"ntk9","ntfy_poll":11,"ntfy_host":"nth11",)"
        R"("mqtt_host":"mqh12","mqtt_user":"mqu13","mqtt_pass":"mqp14","name":"nam15",)"
        R"("hostname":"hnm16","hosts":"hst17","tz":19,"bat_sda":20,)"
        R"("lang":"pt","ota_auto":false,"api_token":"api22","cf_token":"cft23",)"
        R"("cf_zone":"cfz24","cf_host":"cfh25","cf_record":"cfr26","cf_account":"cfa27",)"
        R"("relay_host":"rlh28","relay_key":"rlk29","relay_ver":31,"relay_name":"rln31",)"
        R"("relay_zone":"rlz32","sensor":"sen33","sensor_pin":35,"sensor_high":false,)"
        R"("sensor_on":"son36","sensor_off":"sof37","metric":39,"unit":40,)"
        R"("tank_empty":41,"tank_full":42,"tank_l":43,"ppl":44,)"
        R"("pulse":45,"notify":true,"actuator":"act46","actuator_pin":48,)"
        R"("actuator_high":true,"angle_on":50,"angle_off":51,"duty_on":52,)"
        R"("duty_off":53,"leds":54,"max_on":55,"restore":true,)"
        R"("rule":false,"above":true,"threshold":59,"margin":60,)"
        R"("toggle":true,"linear":true,"threshold_hi":63,"window":true,)"
        R"("win_start":65,"win_end":66)"
        "}");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "Configuration loaded"));
    TEST_ASSERT_FALSE(logHas(logger, "Configuration saved"));   // read, not rewritten
    TEST_ASSERT_TRUE(store.save());
    TEST_ASSERT_EQUAL_STRING("{\"v\":3,"
        R"("usr":"usr0","pwd":"pbkdf2:32df4bf98374fb9500b6e2d939aefb9c:a09a1183612299e514bbc7bdcc881f941231a72c","otp":"00112233445566778899aabbccddeeff00112233","ots":4,)"
        R"("otb":5,"sid":"sid5","wpw":"wpw6","ntt":"ntt7",)"
        R"("ntc":"ntc8","ntk":"ntk9","ntp":11,"nth":"nth11",)"
        R"("mqh":"mqh12","mqu":"mqu13","mqp":"mqp14","nam":"nam15",)"
        R"("hnm":"hnm16","hst":"hst17","tzo":19,"bat":20,)"
        R"("lng":"pt","ota":false,"api":"api22","cft":"cft23",)"
        R"("cfz":"cfz24","cfh":"cfh25","cfr":"cfr26","cfa":"cfa27",)"
        R"("rlh":"rlh28","rlk":"rlk29","rlv":31,"rln":"rln31",)"
        R"("rlz":"rlz32","sen":"sen33","spn":35,"shi":false,)"
        R"("son":"son36","sof":"sof37","met":39,"unt":40,)"
        R"("tke":41,"tkf":42,"tkl":43,"fpl":44,)"
        R"("pls":45,"ntf":true,"act":"act46","apn":48,)"
        R"("ahi":true,"ag1":50,"ag0":51,"dt1":52,)"
        R"("dt0":53,"led":54,"mxo":55,"rst":true,)"
        R"("rul":false,"abv":true,"thd":59,"mrg":60,)"
        R"("tgl":true,"lin":true,"thh":63,"win":true,)"
        R"("wns":65,"wne":66)"
        "}", MockFS::files["/config.json"].c_str());
}

// The check behind the static_assert, also at run time, both ways
void test_keys_are_distinct() {
    TEST_ASSERT_TRUE(ConfigStore::keysDistinct());
    const uint16_t same[] = {1, 2, 3, 2};
    TEST_ASSERT_FALSE(ConfigStore::distinct(same, 4));
    TEST_ASSERT_TRUE(ConfigStore::distinct(same, 3));
}

// An OTA that went back: an unknown type stays stored, the file untouched
void test_newer_version_is_not_rewritten() {
    const char* v4 = R"({"v":4,"sen":"laser","spn":2,"new":1,"x":1})";
    MockFS::put("/config.json", v4);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "v4, newer than"));
    TEST_ASSERT_EQUAL_STRING("laser", config.iot.sensor);
    TEST_ASSERT_EQUAL(2, config.iot.sensorPin);
    TEST_ASSERT_EQUAL_STRING(v4, MockFS::files["/config.json"].c_str());
}

void test_ota_auto_persists() {
    config.ota_auto = false;
    TEST_ASSERT_TRUE(store.save());
    config = Config{};
    TEST_ASSERT_TRUE(config.ota_auto);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_FALSE(config.ota_auto);
}

// There used to be up to 5 IDs (cf_records): the first stays, and save writes the new format
void test_load_record_from_old_list() {
    MockFS::put("/config.json", R"({"v":2,"cf_records":["a","b","c"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("a", config.cf_record);
    TEST_ASSERT_TRUE(store.save());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"cfr\":\"a\""));
    TEST_ASSERT_FALSE(contains(MockFS::files["/config.json"], "cf_records"));

    MockFS::put("/config.json", R"({"v":2,"cf_record":"new","cf_records":["old"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("new", config.cf_record);
}

void test_load_non_text_record_becomes_empty() {
    MockFS::put("/config.json", R"({"v":2,"cf_records":[123,"ok"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("", config.cf_record);
    MockFS::put("/config.json", R"({"v":2,"cf_record":5,"cf_records":["ok"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("ok", config.cf_record);
}

void test_full_round_trip() {
    config.webusr = "user";
    config.webpss = "secret123";
    config.ssid   = "MinhaRede";
    config.pass   = "Pass1234";
    strlcpy(config.cf_token,   "tok_abc123",       sizeof(config.cf_token));
    strlcpy(config.cf_zone,    "zone_xyz",         sizeof(config.cf_zone));
    strlcpy(config.cf_host,    "home.example.com", sizeof(config.cf_host));
    strlcpy(config.ntfy_topic, "topic",           sizeof(config.ntfy_topic));
    strlcpy(config.ntfy_host,  "ntfy.example.com", sizeof(config.ntfy_host));
    strlcpy(config.api_token,  "abc123",           sizeof(config.api_token));
    strlcpy(config.cf_record,  "rec_aaa",          sizeof(config.cf_record));
    strlcpy(config.dev_name,   "Garage gate", sizeof(config.dev_name));
    strlcpy(config.hostname,   "portao",           sizeof(config.hostname));
    strlcpy(config.web_hosts,  "esp.example.com",  sizeof(config.web_hosts));
    strlcpy(config.relay_host, "esp-pump.example.com", sizeof(config.relay_host));
    strlcpy(config.relay_secret, "0123abcd", sizeof(config.relay_secret));
    strlcpy(config.cf_account, "01a7362d577a6c3019a474fd6f485823", sizeof(config.cf_account));
    config.relay_ver = 1;
    config.ntfy_poll = 30;
    strlcpy(config.relay_name, "esp-pump", sizeof(config.relay_name));
    strlcpy(config.relay_zone, "023e105f4ecef8ad9ca31a8372d0c353", sizeof(config.relay_zone));
    config.tz = -180;
    config.bat_sda = 3;
    strlcpy(config.lang, "pt", sizeof(config.lang));
    config.iot.window = true;
    config.iot.winStart = 22 * 60;
    config.iot.winEnd = 6 * 60;
    config.iot.unit = 2;
    config.iot.tankEmpty = 150;
    config.iot.tankFull = 25;
    config.iot.tankL = 3000;
    config.iot.ppl = 330;
    config.iot.pulse = 12;
    static const uint8_t key[Totp::KEY_LEN] = {
        '1','2','3','4','5','6','7','8','9','0','1','2','3','4','5','6','7','8','9','0'
    };
    std::memcpy(config.totp, key, sizeof(key));
    config.totpStep = 56670000;
    config.totpBrowserStep = 56670123;

    TEST_ASSERT_TRUE(store.save());
    config = Config{};
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("Garage gate", config.dev_name);
    TEST_ASSERT_EQUAL_STRING("portao",           config.hostname);
    TEST_ASSERT_EQUAL_STRING("esp.example.com",  config.web_hosts);
    TEST_ASSERT_EQUAL_STRING("esp-pump.example.com", config.relay_host);
    TEST_ASSERT_EQUAL_STRING("0123abcd", config.relay_secret);
    TEST_ASSERT_EQUAL_STRING("01a7362d577a6c3019a474fd6f485823", config.cf_account);
    TEST_ASSERT_EQUAL(1, config.relay_ver);
    TEST_ASSERT_EQUAL(30, config.ntfy_poll);
    TEST_ASSERT_EQUAL_STRING("esp-pump", config.relay_name);
    TEST_ASSERT_EQUAL_STRING("023e105f4ecef8ad9ca31a8372d0c353", config.relay_zone);
    TEST_ASSERT_EQUAL(-180, config.tz);
    TEST_ASSERT_EQUAL(3, config.bat_sda);
    TEST_ASSERT_EQUAL_STRING("pt", config.lang);
    TEST_ASSERT_TRUE(config.iot.window);
    TEST_ASSERT_EQUAL(22 * 60, config.iot.winStart);
    TEST_ASSERT_EQUAL(6 * 60, config.iot.winEnd);
    TEST_ASSERT_EQUAL(2, config.iot.unit);
    TEST_ASSERT_EQUAL(150, config.iot.tankEmpty);
    TEST_ASSERT_EQUAL(25, config.iot.tankFull);
    TEST_ASSERT_EQUAL(3000, config.iot.tankL);
    TEST_ASSERT_EQUAL(330, config.iot.ppl);
    TEST_ASSERT_EQUAL(12, config.iot.pulse);

    TEST_ASSERT_EQUAL_STRING("user",             config.webusr.c_str());
    TEST_ASSERT_TRUE(Password::hashed(config.webpss));
    TEST_ASSERT_TRUE(Password::check("secret123", config.webpss));
    TEST_ASSERT_EQUAL_STRING("MinhaRede",        config.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Pass1234",         config.pass.c_str());
    TEST_ASSERT_EQUAL_STRING("tok_abc123",       config.cf_token);
    TEST_ASSERT_EQUAL_STRING("zone_xyz",         config.cf_zone);
    TEST_ASSERT_EQUAL_STRING("home.example.com", config.cf_host);
    TEST_ASSERT_EQUAL_STRING("topic",           config.ntfy_topic);
    TEST_ASSERT_EQUAL_STRING("ntfy.example.com", config.ntfy_host);
    TEST_ASSERT_EQUAL_STRING("abc123",           config.api_token);
    TEST_ASSERT_EQUAL_STRING("rec_aaa",          config.cf_record);
    TEST_ASSERT_EQUAL_MEMORY(key, config.totp, Totp::KEY_LEN);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"],
                              "3132333435363738393031323334353637383930"));
    TEST_ASSERT_EQUAL_UINT32(56670000u, config.totpStep);
    TEST_ASSERT_EQUAL_UINT32(56670123u, config.totpBrowserStep);
}

// Every field at its maximum size fits under the read limit
void test_save_worst_case_fits() {
    config.webusr = std::string(32, 'u');
    config.webpss = std::string(64, 'p');
    config.ssid   = std::string(32, 's');
    config.pass   = std::string(63, 'w');
    std::memset(config.cf_token,   't', sizeof(config.cf_token) - 1);
    std::memset(config.cf_zone,    'z', sizeof(config.cf_zone) - 1);
    std::memset(config.cf_host,    'h', sizeof(config.cf_host) - 1);
    std::memset(config.ntfy_topic, 'n', sizeof(config.ntfy_topic) - 1);
    std::memset(config.ntfy_cmd,   'c', sizeof(config.ntfy_cmd) - 1);
    std::memset(config.ntfy_token, 'k', sizeof(config.ntfy_token) - 1);
    std::memset(config.ntfy_host,  'y', sizeof(config.ntfy_host) - 1);
    std::memset(config.mqtt_host,  'm', sizeof(config.mqtt_host) - 1);
    std::memset(config.mqtt_user,  'u', sizeof(config.mqtt_user) - 1);
    std::memset(config.mqtt_pass,  'p', sizeof(config.mqtt_pass) - 1);
    std::memset(config.cf_record,  'r', sizeof(config.cf_record) - 1);
    std::memset(config.dev_name,   'd', sizeof(config.dev_name) - 1);
    std::memset(config.hostname,   'o', sizeof(config.hostname) - 1);
    std::memset(config.lang,       'l', sizeof(config.lang) - 1);
    std::memset(config.web_hosts,  'w', sizeof(config.web_hosts) - 1);
    std::memset(config.relay_host, 'r', sizeof(config.relay_host) - 1);
    std::memset(config.relay_secret, 'f', sizeof(config.relay_secret) - 1);
    std::memset(config.cf_account, 'a', sizeof(config.cf_account) - 1);
    config.relay_ver = 255;
    std::memset(config.relay_name, 'n', sizeof(config.relay_name) - 1);
    std::memset(config.relay_zone, 'z', sizeof(config.relay_zone) - 1);
    std::memset(config.iot.sensor,   's', sizeof(config.iot.sensor) - 1);
    std::memset(config.iot.actuator, 'a', sizeof(config.iot.actuator) - 1);
    std::memset(config.iot.onText,   'o', sizeof(config.iot.onText) - 1);
    std::memset(config.iot.offText,  'f', sizeof(config.iot.offText) - 1);
    config.iot.unit = 255;
    config.iot.tankEmpty = config.iot.tankFull = config.iot.tankL = config.iot.ppl = config.iot.pulse = 65535;
    std::memset(config.totp, 0xA5, sizeof(config.totp));
    config.totpStep = 0xFFFFFFFFu;

    TEST_ASSERT_TRUE(store.save());
    // The largest possible file stays below the read limit
    TEST_ASSERT_TRUE(MockFS::files["/config.json"].size() < ConfigStore::MAX_FILE);
    config = Config{};
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL(63, (int)std::strlen(config.cf_host));
    TEST_ASSERT_EQUAL(39, (int)std::strlen(config.cf_record));
    TEST_ASSERT_EQUAL(15, (int)std::strlen(config.iot.offText));
    TEST_ASSERT_EQUAL(63, (int)std::strlen(config.mqtt_host));
    TEST_ASSERT_EQUAL(63, (int)std::strlen(config.ntfy_host));
    TEST_ASSERT_EQUAL(31, (int)std::strlen(config.mqtt_user));
    TEST_ASSERT_EQUAL(63, (int)std::strlen(config.mqtt_pass));
    TEST_ASSERT_EQUAL(79, (int)std::strlen(config.relay_host));
    TEST_ASSERT_EQUAL(64, (int)std::strlen(config.relay_secret));
    TEST_ASSERT_EQUAL_UINT8(0xA5, config.totp[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA5, config.totp[Totp::KEY_LEN - 1]);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, config.totpStep);
}

// A config from before the step was kept: 0, so any code is new
void test_totp_step_missing_is_zero() {
    config.totpStep = 123;
    MockFS::put("/config.json", R"({"v":2,"ssid":"Home"})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_UINT32(0u, config.totpStep);
    MockFS::put("/config.json", R"({"v":2,"totp_step":"x"})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_UINT32(0u, config.totpStep);
}

// Missing, short, all-zero or non-text: not enrolled. Uppercase hex is kept.
void test_totp_secret_rejected() {
    uint8_t mark[Totp::KEY_LEN];
    std::memset(mark, 0x5A, sizeof(mark));
    const char* bad[] = {
        R"({"v":2,"totp":"zz"})",
        R"({"v":2,"totp":"0000000000000000000000000000000000000000"})",
        R"({"v":2,"totp":1})",
        R"({"v":2,"ssid":"Home"})",
    };
    for (const char* json : bad) {
        std::memcpy(config.totp, mark, sizeof(mark));
        MockFS::put("/config.json", json);
        TEST_ASSERT_TRUE(store.load());
        TEST_ASSERT_TRUE(Totp::blank(config.totp));
    }
    TEST_ASSERT_EQUAL_STRING("Home", config.ssid.c_str());

    MockFS::put("/config.json",
                R"({"v":2,"totp":"AB00000000000000000000000000000000000001"})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_UINT8(0xAB, config.totp[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, config.totp[19]);
    TEST_ASSERT_EQUAL_UINT8(0x00, config.totp[1]);
}

void test_save_overflow_does_not_write() {
    config.webusr = std::string(4000, 'x');
    TEST_ASSERT_FALSE(store.save());
    TEST_ASSERT_TRUE(logHas(logger, "exceeds the file limit"));
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.size());
}

// A value of the wrong type, out of the field's range or not a whole
// number keeps the default, field by field, like the missing ones
void test_wrong_types_and_ranges_keep_defaults() {
    config.ssid = "old";
    MockFS::put("/config.json", R"({"v":2,"ssid":1,"webusr":null,"ota_auto":1,"tz":40000,)"
        R"("bat_sda":-129,"sensor_pin":1.5,"leds":256,"max_on":-1,"angle_on":"9",)"
        R"("totp_step":-1,"name":["x"],"totp":")" + std::string(50, 'a') + R"("})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("", config.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("admin", config.webusr.c_str());
    TEST_ASSERT_TRUE(config.ota_auto);
    TEST_ASSERT_EQUAL(0, config.tz);
    TEST_ASSERT_EQUAL(-1, config.bat_sda);
    TEST_ASSERT_EQUAL(-1, config.iot.sensorPin);
    TEST_ASSERT_EQUAL(8, config.iot.leds);
    TEST_ASSERT_EQUAL(0, config.iot.maxOn);
    TEST_ASSERT_EQUAL(90, config.iot.angleOn);
    TEST_ASSERT_EQUAL(0, config.iot.unit);
    TEST_ASSERT_EQUAL(100, config.iot.tankEmpty);
    TEST_ASSERT_EQUAL(20, config.iot.tankFull);
    TEST_ASSERT_EQUAL(1000, config.iot.tankL);
    TEST_ASSERT_EQUAL(450, config.iot.ppl);
    TEST_ASSERT_EQUAL(0, config.iot.pulse);
    TEST_ASSERT_EQUAL_UINT32(0u, config.totpStep);
    TEST_ASSERT_EQUAL_STRING("", config.dev_name);
    TEST_ASSERT_TRUE(Totp::blank(config.totp));

    // Each type at the edges of its range
    MockFS::put("/config.json", R"({"v":2,"tz":-32768,"bat_sda":127,"leds":255,"max_on":65535,)"
        R"("threshold":32767,"totp_step":4294967295})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL(-32768, config.tz);
    TEST_ASSERT_EQUAL(127, config.bat_sda);
    TEST_ASSERT_EQUAL(255, config.iot.leds);
    TEST_ASSERT_EQUAL(65535, config.iot.maxOn);
    TEST_ASSERT_EQUAL(32767, config.iot.threshold);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, config.totpStep);
}

// "v" that is not a whole number counts as v1 (migrated); an old list that is
// not a list gives no record
void test_odd_version_and_record_list() {
    MockFS::put("/config.json", R"({"v":"2","cf_records":"abc","ssid":"x"})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "migrated to v3"));
    TEST_ASSERT_EQUAL_STRING("", config.cf_record);
    TEST_ASSERT_EQUAL_STRING("x", config.ssid.c_str());
}

// Text with quotes, backslashes, control characters and accents comes back the same
void test_text_with_special_characters_round_trip() {
    config.ssid = "Casa \"1\" \\ \t\x01 \xc3\xa9";
    strlcpy(config.dev_name, "Port\xc3\xa3o \"A\"", sizeof(config.dev_name));
    TEST_ASSERT_TRUE(store.save());
    config = Config{};
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("Casa \"1\" \\ \t\x01 \xc3\xa9", config.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Port\xc3\xa3o \"A\"", config.dev_name);
}

// A password saved before the hash is replaced on the first boot, and the
// file no longer holds it; a newer file is not rewritten, only read
void test_plain_password_becomes_a_hash() {
    MockFS::put("/config.json", R"({"v":2,"webusr":"admin","webpss":"old-plain-pass"})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(Password::hashed(config.webpss));
    TEST_ASSERT_TRUE(Password::check("old-plain-pass", config.webpss));
    TEST_ASSERT_TRUE(logHas(logger, "now stored as a hash"));
    TEST_ASSERT_FALSE(contains(MockFS::files["/config.json"], "old-plain-pass"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], config.webpss.c_str()));

    // Already a hash: loaded as is, nothing rewritten
    std::string saved = MockFS::files["/config.json"];
    logger = Logger();
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_FALSE(logHas(logger, "now stored as a hash"));
    TEST_ASSERT_FALSE(logHas(logger, "Configuration saved"));
    TEST_ASSERT_EQUAL_STRING(saved.c_str(), MockFS::files["/config.json"].c_str());

    const char* v4 = R"({"v":4,"pwd":"plain-in-v4"})";
    MockFS::put("/config.json", v4);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(Password::check("plain-in-v4", config.webpss));
    TEST_ASSERT_EQUAL_STRING(v4, MockFS::files["/config.json"].c_str());
}

void test_save_open_fails() {
    MockFS::failWriteOpen = true;
    TEST_ASSERT_FALSE(store.save());
    TEST_ASSERT_TRUE(logHas(logger, "Could not save"));
}

void test_save_rename_failure_keeps_original() {
    MockFS::put("/config.json", R"({"ssid":"old"})");
    MockFS::failRename = true;
    config.ssid = "new";

    TEST_ASSERT_FALSE(store.save());
    TEST_ASSERT_TRUE(logHas(logger, "Could not replace"));
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/config.tmp"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "old"));
}

void test_save_atomic_without_temp_file() {
    TEST_ASSERT_TRUE(store.save());
    TEST_ASSERT_EQUAL(1, (int)MockFS::files.count("/config.json"));
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/config.tmp"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_ok);
    RUN_TEST(test_begin_fails);
    RUN_TEST(test_load_false_without_file);
    RUN_TEST(test_load_open_fails);
    RUN_TEST(test_load_false_on_invalid_json);
    RUN_TEST(test_load_file_too_large);
    RUN_TEST(test_load_without_memory);
    RUN_TEST(test_load_at_the_size_limit);
    RUN_TEST(test_load_defaults);
    RUN_TEST(test_iot_v2_round_trip);
    RUN_TEST(test_v1_migration_full);
    RUN_TEST(test_v1_migration_forced_off_and_types_out_of_table);
    RUN_TEST(test_v1_migration_without_sensor_keeps_module_idle);
    RUN_TEST(test_migration_that_cannot_save_keeps_the_config);
    RUN_TEST(test_v2_names_are_read);
    RUN_TEST(test_keys_are_distinct);
    RUN_TEST(test_newer_version_is_not_rewritten);
    RUN_TEST(test_ota_auto_persists);
    RUN_TEST(test_load_record_from_old_list);
    RUN_TEST(test_load_non_text_record_becomes_empty);
    RUN_TEST(test_full_round_trip);
    RUN_TEST(test_totp_secret_rejected);
    RUN_TEST(test_totp_step_missing_is_zero);
    RUN_TEST(test_save_worst_case_fits);
    RUN_TEST(test_save_overflow_does_not_write);
    RUN_TEST(test_wrong_types_and_ranges_keep_defaults);
    RUN_TEST(test_odd_version_and_record_list);
    RUN_TEST(test_text_with_special_characters_round_trip);
    RUN_TEST(test_plain_password_becomes_a_hash);
    RUN_TEST(test_save_open_fails);
    RUN_TEST(test_save_rename_failure_keeps_original);
    RUN_TEST(test_save_atomic_without_temp_file);
    return UNITY_END();
}
