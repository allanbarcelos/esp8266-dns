#include <unity.h>
#include "../test_support.h"
#include "Clock.h"
#include "Config.h"
#include "NtfyNotifier.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    strlcpy(cfg.ntfy_topic, "meu-topico", sizeof(cfg.ntfy_topic));
}
void tearDown() {}

struct Fix {
    Clock            clock{fakeTime};
    PublicIPResolver resolver{logger, clock};
    NtfyNotifier     ntfy{cfg, logger, resolver};
};

void test_sem_envio_sem_topico() {
    cfg.ntfy_topic[0] = '\0';
    Fix f;
    f.ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_sem_relogio_nao_envia() {
    fakeNow = 1000;
    Fix f;
    f.ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "relogio nao sincronizado"));
    TEST_ASSERT_EQUAL_STRING("", cfg.publicIP);
}

void test_envia_no_boot_sem_cloudflare() {
    TEST_ASSERT_EQUAL_STRING("", cfg.cf_token);
    strlcpy(cfg.publicIP, "9.9.9.9", sizeof(cfg.publicIP));
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(0);

    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("https://api4.ipify.org", MockHTTP::requests[0].url.c_str());
    TEST_ASSERT_EQUAL_STRING("GET", MockHTTP::requests[0].method.c_str());
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/meu-topico", MockHTTP::requests[1].url.c_str());
    TEST_ASSERT_EQUAL_STRING("POST", MockHTTP::requests[1].method.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[1].payload, "1.2.3.4"));
    TEST_ASSERT_FALSE(contains(MockHTTP::requests[1].payload, "9.9.9.9"));
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy enviado (200)"));
    TEST_ASSERT_EQUAL(1, MockTLS::insecureCalls);
    TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, MockTLS::lastTrustPem);
}

void test_nao_reenvia_mesmo_ip() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(0);
    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(3, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("GET", MockHTTP::requests[2].method.c_str());
}

void test_reenvia_quando_ip_muda() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(0);
    MockHTTP::push(200, "5.6.7.8");
    f.ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(4, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("POST", MockHTTP::requests[3].method.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[3].payload, "5.6.7.8"));
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", cfg.publicIP);
}

void test_falha_aguarda_intervalo_para_retentar() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(500);
    f.ntfy.tick(1000);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy erro: 500"));

    f.ntfy.tick(1000 + NtfyNotifier::RETRY_INTERVAL - 1);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());

    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(1000 + NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(4, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy enviado (200)"));
}

void test_erro_de_conexao() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::beginResults = {true, false};
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: falha ao conectar"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("GET", MockHTTP::requests[0].method.c_str());
}

void test_codigo_negativo() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(-1);
    f.ntfy.tick(0);
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
    RUN_TEST(test_sem_envio_sem_topico);
    RUN_TEST(test_sem_relogio_nao_envia);
    RUN_TEST(test_envia_no_boot_sem_cloudflare);
    RUN_TEST(test_nao_reenvia_mesmo_ip);
    RUN_TEST(test_reenvia_quando_ip_muda);
    RUN_TEST(test_falha_aguarda_intervalo_para_retentar);
    RUN_TEST(test_erro_de_conexao);
    RUN_TEST(test_codigo_negativo);
    RUN_TEST(test_valida_topico);
    return UNITY_END();
}
