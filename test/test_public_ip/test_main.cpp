// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Clock.h"
#include "PublicIPResolver.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;

void setUp() {
    resetMocks();
    logger = Logger();
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
}
void tearDown() {}

struct Fix {
    Clock            clock{fakeTime};
    PublicIPResolver resolver{logger, clock};
};

void test_primeiro_servico_ok() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", out);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("https://api4.ipify.org", MockHTTP::calledURLs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("ESP8266", MockHTTP::requests[0].headers[0].second.c_str());
    TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, MockTLS::lastTrustPem);
    TEST_ASSERT_EQUAL((long)fakeNow, (long)MockTLS::lastTime);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
    // wrapper e contexto BearSSL recebem o mesmo orçamento
    TEST_ASSERT_EQUAL(2, MockTLS::timeoutCalls);
    TEST_ASSERT_EQUAL(PublicIPResolver::CONNECT_TIMEOUT, MockTLS::lastTimeoutMs);
    TEST_ASSERT_EQUAL(TlsClient::RX_BUF, MockTLS::bufIn);
    TEST_ASSERT_EQUAL(TlsClient::TX_BUF, MockTLS::bufOut);
    // Só ECDHE + AES-GCM: as outras cifras nem estão no firmware (TlsTrim.h)
    const std::vector<uint16_t> gcm = {0xC02B, 0xC02F, 0xC02C, 0xC030};
    TEST_ASSERT_TRUE(MockTLS::suites == gcm);
}

void test_memoria_insuficiente_nao_consulta() {
    Fix f;
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    char out[16] = "x";
    TEST_ASSERT_FALSE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("x", out);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "memoria insuficiente"));

    logger = Logger();
    ESP._heap = 50000;
    ESP._maxBlock = PublicIPResolver::MIN_TLS_BLOCK - 1;
    TEST_ASSERT_FALSE(f.resolver.resolve(out, sizeof(out), PublicIPResolver::CACHE_TTL));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "memoria insuficiente"));
}

void test_tls_indisponivel() {
    Fix f;
    MockTLS::nullOwned = true;
    char out[16] = "x";
    TEST_ASSERT_FALSE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("x", out);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "TLS indisponivel"));
}

void test_aceita_espacos_e_quebra_de_linha() {
    Fix f;
    MockHTTP::push(200, "  5.6.7.8\n");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", out);
}

void test_falha_de_conexao_tenta_proximo() {
    Fix f;
    MockHTTP::failNextBegin();
    MockHTTP::push(200, "9.9.9.9");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("9.9.9.9", out);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("https://icanhazip.com", MockHTTP::calledURLs[1].c_str());
    TEST_ASSERT_TRUE(logHas(logger, "falha ao conectar"));
}

void test_http_erro_tenta_proximo() {
    Fix f;
    MockHTTP::push(503);
    MockHTTP::push(200, "8.8.4.4");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("8.8.4.4", out);
    TEST_ASSERT_TRUE(logHas(logger, "HTTP 503"));
}

void test_sem_conexao_tenta_proximo() {
    Fix f;
    MockHTTP::push(-1);
    MockHTTP::push(200, "7.7.7.7");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("7.7.7.7", out);
    TEST_ASSERT_TRUE(logHas(logger, "sem conexao (-1)"));
    TEST_ASSERT_FALSE(logHas(logger, "HTTP -1"));
}

void test_resposta_invalida_tenta_proximo() {
    Fix f;
    MockHTTP::push(200, "<html>erro</html>");
    MockHTTP::push(200, "300.1.1.1");
    MockHTTP::push(200, "10.0.0.1");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", out);
    TEST_ASSERT_EQUAL_STRING("https://ifconfig.co/ip", MockHTTP::calledURLs[2].c_str());
    TEST_ASSERT_TRUE(logHas(logger, "resposta invalida"));
}

void test_todos_falham_nao_altera_saida() {
    Fix f;
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(500);
    char out[16] = "anterior";
    TEST_ASSERT_FALSE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("anterior", out);
    TEST_ASSERT_EQUAL(NUM_IP_SERVICES, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "Todos os servicos"));
}

void test_buffer_pequeno_rejeita() {
    Fix f;
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(200, "100.100.100.100");
    char out[8] = "x";
    TEST_ASSERT_FALSE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("x", out);
}

void test_sem_relogio_nao_consulta() {
    fakeNow = 1000;
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    char out[16] = "x";
    TEST_ASSERT_FALSE(f.resolver.resolve(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("x", out);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "relogio nao sincronizado"));
}

void test_cache_reutiliza_sem_rede() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 1000));
    char again[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(again, sizeof(again), 1000 + PublicIPResolver::CACHE_TTL - 1));
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", again);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_cache_nao_cabe_no_buffer() {
    Fix f;
    MockHTTP::push(200, "100.100.100.100");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 1000));
    char small[8] = "y";
    TEST_ASSERT_FALSE(f.resolver.resolve(small, sizeof(small), 1000));
    TEST_ASSERT_EQUAL_STRING("y", small);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_cache_expira_e_busca_de_novo() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, "5.6.7.8");
    char out[16] = "";
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 1000));
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 1000 + PublicIPResolver::CACHE_TTL));
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", out);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
}

void test_falha_recente_nao_repete() {
    Fix f;
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(500);
    char out[16] = "x";
    TEST_ASSERT_FALSE(f.resolver.resolve(out, sizeof(out), 1000));
    TEST_ASSERT_FALSE(f.resolver.resolve(out, sizeof(out), 1000 + PublicIPResolver::CACHE_TTL - 1));
    TEST_ASSERT_EQUAL(NUM_IP_SERVICES, (int)MockHTTP::calledURLs.size());

    MockHTTP::push(200, "1.2.3.4");
    TEST_ASSERT_TRUE(f.resolver.resolve(out, sizeof(out), 1000 + PublicIPResolver::CACHE_TTL));
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", out);
    TEST_ASSERT_EQUAL(NUM_IP_SERVICES + 1, (int)MockHTTP::calledURLs.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_primeiro_servico_ok);
    RUN_TEST(test_memoria_insuficiente_nao_consulta);
    RUN_TEST(test_tls_indisponivel);
    RUN_TEST(test_aceita_espacos_e_quebra_de_linha);
    RUN_TEST(test_falha_de_conexao_tenta_proximo);
    RUN_TEST(test_http_erro_tenta_proximo);
    RUN_TEST(test_sem_conexao_tenta_proximo);
    RUN_TEST(test_resposta_invalida_tenta_proximo);
    RUN_TEST(test_todos_falham_nao_altera_saida);
    RUN_TEST(test_buffer_pequeno_rejeita);
    RUN_TEST(test_sem_relogio_nao_consulta);
    RUN_TEST(test_cache_reutiliza_sem_rede);
    RUN_TEST(test_cache_nao_cabe_no_buffer);
    RUN_TEST(test_cache_expira_e_busca_de_novo);
    RUN_TEST(test_falha_recente_nao_repete);
    return UNITY_END();
}
