// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "ConfigStore.h"

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
    TEST_ASSERT_TRUE(logHas(logger, "LittleFS inicializado"));
}

void test_begin_falha() {
    MockFS::beginOk = false;
    TEST_ASSERT_FALSE(store.begin());
    TEST_ASSERT_TRUE(logHas(logger, "Falha ao montar"));
}

void test_load_retorna_false_sem_arquivo() {
    TEST_ASSERT_FALSE(store.load());
}

void test_load_falha_ao_abrir() {
    MockFS::put("/config.json", "{}");
    MockFS::failReadOpen = true;
    TEST_ASSERT_FALSE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "Erro ao abrir"));
}

void test_load_retorna_false_json_invalido() {
    MockFS::put("/config.json", "{ broken json !!!");
    TEST_ASSERT_FALSE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "Erro JSON"));
}

// Arquivo maior do que qualquer save() grava: nem tenta ler
void test_load_arquivo_grande_demais() {
    MockFS::put("/config.json", std::string(ConfigStore::MAX_FILE, ' '));
    TEST_ASSERT_FALSE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "config.json ilegivel (4096 bytes)"));
}

// Sem memória para o texto: falha limpa, sem tocar na configuração
void test_load_sem_memoria() {
    MockFS::put("/config.json", R"({"ssid":"rede"})");
    mock::failMallocSize = std::strlen(R"({"ssid":"rede"})") + 1;
    TEST_ASSERT_FALSE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "config.json ilegivel (15 bytes)"));
    TEST_ASSERT_EQUAL_STRING("", config.ssid.c_str());
}

// Logo abaixo do teto ainda é lido
void test_load_no_limite_do_tamanho() {
    std::string json = R"({"ssid":"rede"})";
    json += std::string(ConfigStore::MAX_FILE - 1 - json.size(), ' ');
    MockFS::put("/config.json", json);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("rede", config.ssid.c_str());
}

void test_load_defaults() {
    MockFS::put("/config.json", R"({"v":2,"ssid":"net"})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("admin", config.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.webpss.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.pass.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.cf_record);
    TEST_ASSERT_TRUE(config.ota_auto);
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
    // Já na versão atual: nada é regravado
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/config.v1.json"));
}

// Formato v2: todos os campos voltam iguais
void test_iot_v2_ida_e_volta() {
    IoTConfig& t = config.iot;
    strlcpy(t.sensor, "dht22", sizeof(t.sensor));
    t.sensorPin = 2;
    t.sensorHigh = false;
    strlcpy(t.onText, "molhado", sizeof(t.onText));
    strlcpy(t.offText, "seco", sizeof(t.offText));
    t.metric = 1;
    t.notify = true;
    strlcpy(t.actuator, "ventilador", sizeof(t.actuator));
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
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"v\":2"));

    config = Config{};
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "Configuracao carregada"));
    TEST_ASSERT_EQUAL_STRING("dht22", t.sensor);
    TEST_ASSERT_EQUAL(2, t.sensorPin);
    TEST_ASSERT_FALSE(t.sensorHigh);
    TEST_ASSERT_EQUAL_STRING("molhado", t.onText);
    TEST_ASSERT_EQUAL_STRING("seco", t.offText);
    TEST_ASSERT_EQUAL(1, t.metric);
    TEST_ASSERT_TRUE(t.notify);
    TEST_ASSERT_EQUAL_STRING("ventilador", t.actuator);
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

// Configuração de antes do catálogo por identificador (sem "v")
static const char* V1 = R"({"ssid":"net","iot_sensor":2,"iot_module":0,
    "iot_sensor_type":2,"iot_module_type":6,"iot_sensor_high":false,
    "iot_module_high":true,"iot_notify":true,"iot_metric":1,"iot_above":true,
    "iot_threshold":30,"iot_margin":2,"iot_angle_on":120,"iot_angle_off":5,
    "iot_duty_on":70,"iot_duty_off":10,"iot_manual":1,"iot_toggle":true,
    "iot_linear":true,"iot_threshold_hi":40})";

void test_migracao_v1_completa() {
    MockFS::put("/config.json", V1);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "migrada para v2"));
    const IoTConfig& t = config.iot;
    TEST_ASSERT_EQUAL_STRING("dht22", t.sensor);
    TEST_ASSERT_EQUAL(2, t.sensorPin);
    TEST_ASSERT_FALSE(t.sensorHigh);
    TEST_ASSERT_EQUAL_STRING("ventilador", t.actuator);
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

    // "Comando: ligado" vira estado manual guardado, restaurado no boot
    TEST_ASSERT_TRUE(t.restore);
    IoTState st;
    TEST_ASSERT_TRUE(st.load());
    TEST_ASSERT_TRUE(st.manual);
    TEST_ASSERT_TRUE(st.on);
    TEST_ASSERT_EQUAL(70, st.level);
    TEST_ASSERT_EQUAL(120, st.angle);

    // Original guardado; arquivo novo já na v2
    TEST_ASSERT_EQUAL_STRING(V1, MockFS::files["/config.v1.json"].c_str());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"v\":2"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"actuator\":\"ventilador\""));
    TEST_ASSERT_EQUAL_STRING("net", config.ssid.c_str());
}

void test_migracao_v1_desligado_forcado_e_tipos_fora_da_tabela() {
    MockFS::put("/config.json", R"({"iot_sensor":3,"iot_module":1,
        "iot_sensor_type":99,"iot_module_type":99,"iot_manual":2})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("digital", config.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("rele", config.iot.actuator);
    IoTState st;
    TEST_ASSERT_TRUE(st.load());
    TEST_ASSERT_TRUE(st.manual);
    TEST_ASSERT_FALSE(st.on);
}

void test_migracao_v1_sem_sensor_o_modulo_continua_parado() {
    // v1 só acionava o módulo com sensor: sem sensor, nada passa a acionar
    MockFS::put("/config.json", R"({"iot_sensor":-1,"iot_module":0,"iot_manual":1})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("", config.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("", config.iot.actuator);
    TEST_ASSERT_EQUAL(-1, config.iot.actuatorPin);
    TEST_ASSERT_FALSE(config.iot.restore);
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/iot.state"));

    // Mesmo pino no sensor e no módulo: só o sensor
    MockFS::reset();
    MockFS::put("/config.json", R"({"iot_sensor":2,"iot_module":2})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("digital", config.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("", config.iot.actuator);

    // Só o sensor
    MockFS::reset();
    MockFS::put("/config.json", R"({"iot_sensor":2})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("digital", config.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("", config.iot.actuator);

    // Sem comando fixo, sem estado guardado
    MockFS::reset();
    MockFS::put("/config.json", R"({"iot_sensor":2,"iot_module":0})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("rele", config.iot.actuator);
    TEST_ASSERT_FALSE(config.iot.restore);
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/iot.state"));
}

void test_migracao_sem_conseguir_gravar_segue_com_a_config() {
    MockFS::put("/config.json", V1);
    MockFS::failWriteOpen = true;
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("dht22", config.iot.sensor);
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/config.v1.json"));
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/iot.state"));
    TEST_ASSERT_TRUE(logHas(logger, "Erro ao salvar"));
    TEST_ASSERT_EQUAL_STRING(V1, MockFS::files["/config.json"].c_str());
}

// OTA que voltou atrás: tipo desconhecido fica guardado, arquivo intacto
void test_versao_mais_nova_nao_e_regravada() {
    const char* v3 = R"({"v":3,"sensor":"laser","sensor_pin":2,"novo":1})";
    MockFS::put("/config.json", v3);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_TRUE(logHas(logger, "v3, mais nova"));
    TEST_ASSERT_EQUAL_STRING("laser", config.iot.sensor);
    TEST_ASSERT_EQUAL(2, config.iot.sensorPin);
    TEST_ASSERT_EQUAL_STRING(v3, MockFS::files["/config.json"].c_str());
}

void test_ota_auto_persiste() {
    config.ota_auto = false;
    TEST_ASSERT_TRUE(store.save());
    config = Config{};
    TEST_ASSERT_TRUE(config.ota_auto);
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_FALSE(config.ota_auto);
}

// Antes eram até 5 IDs (cf_records): fica o primeiro, e o save grava o novo
void test_load_record_da_lista_antiga() {
    MockFS::put("/config.json", R"({"v":2,"cf_records":["a","b","c"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("a", config.cf_record);
    TEST_ASSERT_TRUE(store.save());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"cf_record\":\"a\""));
    TEST_ASSERT_FALSE(contains(MockFS::files["/config.json"], "cf_records"));

    MockFS::put("/config.json", R"({"v":2,"cf_record":"novo","cf_records":["velho"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("novo", config.cf_record);
}

void test_load_record_nao_texto_vira_vazio() {
    MockFS::put("/config.json", R"({"v":2,"cf_records":[123,"ok"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("", config.cf_record);
    MockFS::put("/config.json", R"({"v":2,"cf_record":5,"cf_records":["ok"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("ok", config.cf_record);
}

void test_roundtrip_completo() {
    config.webusr = "user";
    config.webpss = "secret123";
    config.ssid   = "MinhaRede";
    config.pass   = "Senha123";
    strlcpy(config.cf_token,   "tok_abc123",       sizeof(config.cf_token));
    strlcpy(config.cf_zone,    "zone_xyz",         sizeof(config.cf_zone));
    strlcpy(config.cf_host,    "home.example.com", sizeof(config.cf_host));
    strlcpy(config.ntfy_topic, "topico",           sizeof(config.ntfy_topic));
    strlcpy(config.api_token,  "abc123",           sizeof(config.api_token));
    strlcpy(config.cf_record,  "rec_aaa",          sizeof(config.cf_record));
    strlcpy(config.dev_name,   "Portão da garagem", sizeof(config.dev_name));
    strlcpy(config.hostname,   "portao",           sizeof(config.hostname));
    config.tz = -180;
    config.iot.window = true;
    config.iot.winStart = 22 * 60;
    config.iot.winEnd = 6 * 60;

    TEST_ASSERT_TRUE(store.save());
    config = Config{};
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("Portão da garagem", config.dev_name);
    TEST_ASSERT_EQUAL_STRING("portao",           config.hostname);
    TEST_ASSERT_EQUAL(-180, config.tz);
    TEST_ASSERT_TRUE(config.iot.window);
    TEST_ASSERT_EQUAL(22 * 60, config.iot.winStart);
    TEST_ASSERT_EQUAL(6 * 60, config.iot.winEnd);

    TEST_ASSERT_EQUAL_STRING("user",             config.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("secret123",        config.webpss.c_str());
    TEST_ASSERT_EQUAL_STRING("MinhaRede",        config.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Senha123",         config.pass.c_str());
    TEST_ASSERT_EQUAL_STRING("tok_abc123",       config.cf_token);
    TEST_ASSERT_EQUAL_STRING("zone_xyz",         config.cf_zone);
    TEST_ASSERT_EQUAL_STRING("home.example.com", config.cf_host);
    TEST_ASSERT_EQUAL_STRING("topico",           config.ntfy_topic);
    TEST_ASSERT_EQUAL_STRING("abc123",           config.api_token);
    TEST_ASSERT_EQUAL_STRING("rec_aaa",          config.cf_record);
}

// Todos os campos no tamanho máximo devem caber no documento JSON
void test_save_pior_caso_cabe() {
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
    std::memset(config.mqtt_host,  'm', sizeof(config.mqtt_host) - 1);
    std::memset(config.mqtt_user,  'u', sizeof(config.mqtt_user) - 1);
    std::memset(config.mqtt_pass,  'p', sizeof(config.mqtt_pass) - 1);
    std::memset(config.cf_record,  'r', sizeof(config.cf_record) - 1);
    std::memset(config.dev_name,   'd', sizeof(config.dev_name) - 1);
    std::memset(config.hostname,   'o', sizeof(config.hostname) - 1);
    std::memset(config.iot.sensor,   's', sizeof(config.iot.sensor) - 1);
    std::memset(config.iot.actuator, 'a', sizeof(config.iot.actuator) - 1);
    std::memset(config.iot.onText,   'o', sizeof(config.iot.onText) - 1);
    std::memset(config.iot.offText,  'f', sizeof(config.iot.offText) - 1);

    TEST_ASSERT_TRUE(store.save());
    // O maior arquivo possível fica abaixo do teto de leitura
    TEST_ASSERT_TRUE(MockFS::files["/config.json"].size() < ConfigStore::MAX_FILE);
    config = Config{};
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL(63, (int)std::strlen(config.cf_host));
    TEST_ASSERT_EQUAL(39, (int)std::strlen(config.cf_record));
    TEST_ASSERT_EQUAL(15, (int)std::strlen(config.iot.offText));
    TEST_ASSERT_EQUAL(63, (int)std::strlen(config.mqtt_host));
    TEST_ASSERT_EQUAL(31, (int)std::strlen(config.mqtt_user));
    TEST_ASSERT_EQUAL(63, (int)std::strlen(config.mqtt_pass));
}

void test_save_overflow_nao_grava() {
    config.webusr = std::string(4000, 'x');
    TEST_ASSERT_FALSE(store.save());
    TEST_ASSERT_TRUE(logHas(logger, "excede capacidade"));
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.size());
}

void test_save_falha_ao_abrir() {
    MockFS::failWriteOpen = true;
    TEST_ASSERT_FALSE(store.save());
    TEST_ASSERT_TRUE(logHas(logger, "Erro ao salvar"));
}

void test_save_falha_no_rename_preserva_original() {
    MockFS::put("/config.json", R"({"ssid":"antigo"})");
    MockFS::failRename = true;
    config.ssid = "novo";

    TEST_ASSERT_FALSE(store.save());
    TEST_ASSERT_TRUE(logHas(logger, "Erro ao substituir"));
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/config.tmp"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "antigo"));
}

void test_save_atomico_sem_temporario() {
    TEST_ASSERT_TRUE(store.save());
    TEST_ASSERT_EQUAL(1, (int)MockFS::files.count("/config.json"));
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.count("/config.tmp"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_ok);
    RUN_TEST(test_begin_falha);
    RUN_TEST(test_load_retorna_false_sem_arquivo);
    RUN_TEST(test_load_falha_ao_abrir);
    RUN_TEST(test_load_retorna_false_json_invalido);
    RUN_TEST(test_load_arquivo_grande_demais);
    RUN_TEST(test_load_sem_memoria);
    RUN_TEST(test_load_no_limite_do_tamanho);
    RUN_TEST(test_load_defaults);
    RUN_TEST(test_iot_v2_ida_e_volta);
    RUN_TEST(test_migracao_v1_completa);
    RUN_TEST(test_migracao_v1_desligado_forcado_e_tipos_fora_da_tabela);
    RUN_TEST(test_migracao_v1_sem_sensor_o_modulo_continua_parado);
    RUN_TEST(test_migracao_sem_conseguir_gravar_segue_com_a_config);
    RUN_TEST(test_versao_mais_nova_nao_e_regravada);
    RUN_TEST(test_ota_auto_persiste);
    RUN_TEST(test_load_record_da_lista_antiga);
    RUN_TEST(test_load_record_nao_texto_vira_vazio);
    RUN_TEST(test_roundtrip_completo);
    RUN_TEST(test_save_pior_caso_cabe);
    RUN_TEST(test_save_overflow_nao_grava);
    RUN_TEST(test_save_falha_ao_abrir);
    RUN_TEST(test_save_falha_no_rename_preserva_original);
    RUN_TEST(test_save_atomico_sem_temporario);
    return UNITY_END();
}
