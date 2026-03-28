#include <unity.h>
#include "Config.h"
#include "Logger.h"
#include "ConfigStore.h"

static Logger     logger;
static Config     config;
static ConfigStore store(config, logger);

void setUp() {
    MockFS::reset();
    config = Config{};
    config.cf_record_count = 0;
    config.lastDnsUpdate   = 0;
    std::memset(config.publicIP,   0, sizeof(config.publicIP));
    std::memset(config.cf_token,   0, sizeof(config.cf_token));
    std::memset(config.cf_zone,    0, sizeof(config.cf_zone));
    std::memset(config.cf_records, 0, sizeof(config.cf_records));
    std::memset(config.cf_host,    0, sizeof(config.cf_host));
}
void tearDown() {}

// ── Testes ───────────────────────────────────────────────────────────────────

void test_load_retorna_false_sem_arquivo() {
    bool ok = store.load();
    TEST_ASSERT_FALSE(ok);
}

void test_load_retorna_false_json_invalido() {
    MockFS::put("/config.json", "{ broken json !!!");
    bool ok = store.load();
    TEST_ASSERT_FALSE(ok);
}

void test_load_defaults_webusr() {
    // JSON sem campo webusr → deve usar "admin"
    MockFS::put("/config.json", R"({"ssid":"net","pass":"pw"})");
    store.load();
    TEST_ASSERT_EQUAL_STRING("admin", config.webusr.c_str());
}

void test_roundtrip_ssid_pass() {
    config.ssid = "MinhaRede";
    config.pass = "Senha123";
    store.save();

    config.ssid = "";
    config.pass = "";
    store.load();

    TEST_ASSERT_EQUAL_STRING("MinhaRede", config.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Senha123",  config.pass.c_str());
}

void test_roundtrip_cloudflare() {
    std::strncpy(config.cf_token, "tok_abc123",  sizeof(config.cf_token));
    std::strncpy(config.cf_zone,  "zone_xyz",    sizeof(config.cf_zone));
    std::strncpy(config.cf_host,  "home.example.com", sizeof(config.cf_host));

    std::strncpy(config.cf_records[0], "rec_aaa", sizeof(config.cf_records[0]));
    std::strncpy(config.cf_records[1], "rec_bbb", sizeof(config.cf_records[1]));
    config.cf_record_count = 2;

    store.save();

    // Reset campos Cloudflare
    std::memset(config.cf_token,   0, sizeof(config.cf_token));
    std::memset(config.cf_zone,    0, sizeof(config.cf_zone));
    std::memset(config.cf_host,    0, sizeof(config.cf_host));
    std::memset(config.cf_records, 0, sizeof(config.cf_records));
    config.cf_record_count = 0;

    store.load();

    TEST_ASSERT_EQUAL_STRING("tok_abc123",       config.cf_token);
    TEST_ASSERT_EQUAL_STRING("zone_xyz",         config.cf_zone);
    TEST_ASSERT_EQUAL_STRING("home.example.com", config.cf_host);
    TEST_ASSERT_EQUAL(2,                         config.cf_record_count);
    TEST_ASSERT_EQUAL_STRING("rec_aaa",          config.cf_records[0]);
    TEST_ASSERT_EQUAL_STRING("rec_bbb",          config.cf_records[1]);
}

void test_roundtrip_credenciais_painel() {
    config.webusr = "admin";
    config.webpss = "secret";
    store.save();

    config.webusr = "";
    config.webpss = "";
    store.load();

    TEST_ASSERT_EQUAL_STRING("admin",  config.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("secret", config.webpss.c_str());
}

void test_save_cria_arquivo() {
    store.save();
    TEST_ASSERT_TRUE(MockFS::files.count("/config.json") > 0);
}

// ── Runner ────────────────────────────────────────────────────────────────────
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_load_retorna_false_sem_arquivo);
    RUN_TEST(test_load_retorna_false_json_invalido);
    RUN_TEST(test_load_defaults_webusr);
    RUN_TEST(test_roundtrip_ssid_pass);
    RUN_TEST(test_roundtrip_cloudflare);
    RUN_TEST(test_roundtrip_credenciais_painel);
    RUN_TEST(test_save_cria_arquivo);
    return UNITY_END();
}
