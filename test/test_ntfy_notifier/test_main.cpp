#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "NtfyNotifier.h"

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    strlcpy(cfg.ntfy_topic, "meu-topico", sizeof(cfg.ntfy_topic));
    strlcpy(cfg.publicIP,   "1.2.3.4",    sizeof(cfg.publicIP));
}
void tearDown() {}

void test_sem_envio_sem_ip() {
    cfg.publicIP[0] = '\0';
    NtfyNotifier ntfy(cfg, logger);
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_sem_envio_sem_topico() {
    cfg.ntfy_topic[0] = '\0';
    NtfyNotifier ntfy(cfg, logger);
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_envia_no_boot_ao_ter_ip() {
    NtfyNotifier ntfy(cfg, logger);
    ntfy.tick(0);

    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/meu-topico", MockHTTP::requests[0].url.c_str());
    TEST_ASSERT_EQUAL_STRING("POST", MockHTTP::requests[0].method.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "1.2.3.4"));
    TEST_ASSERT_TRUE(logHas(logger, "ntfy enviado (200)"));
}

void test_nao_reenvia_mesmo_ip() {
    NtfyNotifier ntfy(cfg, logger);
    ntfy.tick(0);
    ntfy.tick(NtfyNotifier::RETRY_INTERVAL * 100);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_reenvia_quando_ip_muda() {
    NtfyNotifier ntfy(cfg, logger);
    ntfy.tick(0);
    strlcpy(cfg.publicIP, "5.6.7.8", sizeof(cfg.publicIP));
    ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[1].payload, "5.6.7.8"));
}

void test_falha_aguarda_intervalo_para_retentar() {
    NtfyNotifier ntfy(cfg, logger);
    MockHTTP::push(500);
    ntfy.tick(1000);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy erro: 500"));

    ntfy.tick(1000 + NtfyNotifier::RETRY_INTERVAL - 1);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());

    ntfy.tick(1000 + NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
}

void test_erro_de_conexao() {
    NtfyNotifier ntfy(cfg, logger);
    MockHTTP::failNextBegin();
    ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: falha ao conectar"));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::requests.size());
}

void test_codigo_negativo() {
    NtfyNotifier ntfy(cfg, logger);
    MockHTTP::push(-1);
    ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy erro: -1"));
}

void test_valida_topico() {
    TEST_ASSERT_TRUE(NtfyNotifier::isValidTopic("abc_DEF-123"));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic(""));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic(String(std::string(64, 'a'))));
    TEST_ASSERT_TRUE(NtfyNotifier::isValidTopic(String(std::string(63, 'a'))));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic("a/b"));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic("a b"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sem_envio_sem_ip);
    RUN_TEST(test_sem_envio_sem_topico);
    RUN_TEST(test_envia_no_boot_ao_ter_ip);
    RUN_TEST(test_nao_reenvia_mesmo_ip);
    RUN_TEST(test_reenvia_quando_ip_muda);
    RUN_TEST(test_falha_aguarda_intervalo_para_retentar);
    RUN_TEST(test_erro_de_conexao);
    RUN_TEST(test_codigo_negativo);
    RUN_TEST(test_valida_topico);
    return UNITY_END();
}
