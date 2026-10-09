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

void test_load_defaults() {
    MockFS::put("/config.json", R"({"ssid":"net"})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL_STRING("admin", config.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.webpss.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.pass.c_str());
    TEST_ASSERT_EQUAL(0, config.cf_record_count);
}

void test_load_limita_quantidade_de_records() {
    MockFS::put("/config.json",
        R"({"cf_records":["a","b","c","d","e","f","g"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL(MAX_RECORDS, config.cf_record_count);
    TEST_ASSERT_EQUAL_STRING("e", config.cf_records[MAX_RECORDS - 1]);
}

void test_load_record_nao_texto_vira_vazio() {
    MockFS::put("/config.json", R"({"cf_records":[123,"ok"]})");
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL(2, config.cf_record_count);
    TEST_ASSERT_EQUAL_STRING("", config.cf_records[0]);
    TEST_ASSERT_EQUAL_STRING("ok", config.cf_records[1]);
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
    strlcpy(config.cf_records[0], "rec_aaa", sizeof(config.cf_records[0]));
    strlcpy(config.cf_records[1], "rec_bbb", sizeof(config.cf_records[1]));
    config.cf_record_count = 2;

    TEST_ASSERT_TRUE(store.save());
    config = Config{};
    TEST_ASSERT_TRUE(store.load());

    TEST_ASSERT_EQUAL_STRING("user",             config.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("secret123",        config.webpss.c_str());
    TEST_ASSERT_EQUAL_STRING("MinhaRede",        config.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Senha123",         config.pass.c_str());
    TEST_ASSERT_EQUAL_STRING("tok_abc123",       config.cf_token);
    TEST_ASSERT_EQUAL_STRING("zone_xyz",         config.cf_zone);
    TEST_ASSERT_EQUAL_STRING("home.example.com", config.cf_host);
    TEST_ASSERT_EQUAL_STRING("topico",           config.ntfy_topic);
    TEST_ASSERT_EQUAL(2,                         config.cf_record_count);
    TEST_ASSERT_EQUAL_STRING("rec_aaa",          config.cf_records[0]);
    TEST_ASSERT_EQUAL_STRING("rec_bbb",          config.cf_records[1]);
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
    for (int i = 0; i < MAX_RECORDS; i++)
        std::memset(config.cf_records[i], 'r', sizeof(config.cf_records[0]) - 1);
    config.cf_record_count = MAX_RECORDS;

    TEST_ASSERT_TRUE(store.save());
    config = Config{};
    TEST_ASSERT_TRUE(store.load());
    TEST_ASSERT_EQUAL(63, (int)std::strlen(config.cf_host));
    TEST_ASSERT_EQUAL(MAX_RECORDS, config.cf_record_count);
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
    RUN_TEST(test_load_defaults);
    RUN_TEST(test_load_limita_quantidade_de_records);
    RUN_TEST(test_load_record_nao_texto_vira_vazio);
    RUN_TEST(test_roundtrip_completo);
    RUN_TEST(test_save_pior_caso_cabe);
    RUN_TEST(test_save_overflow_nao_grava);
    RUN_TEST(test_save_falha_ao_abrir);
    RUN_TEST(test_save_falha_no_rename_preserva_original);
    RUN_TEST(test_save_atomico_sem_temporario);
    return UNITY_END();
}
