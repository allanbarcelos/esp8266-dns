// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Clock.h"
#include "PublicIPResolver.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
}
void tearDown() {}

struct Fix {
    Clock            clock{fakeTime};
    PublicIPResolver resolver{cfg, logger, clock};
};

static const unsigned long T0 = 1000;
static const unsigned long CHECK = PublicIPResolver::CHECK_INTERVAL;
static const unsigned long RETRY = PublicIPResolver::RETRY_INTERVAL;

static int logCount(const char* needle) {
    int n = 0;
    for (uint8_t i = 0; i < logger.count(); i++)
        if (std::strstr(logger.entry(logger.startIndex() + i), needle)) n++;
    return n;
}

void test_primeiro_servico_ok() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);
    TEST_ASSERT_EQUAL(T0, cfg.ipChanged);
    TEST_ASSERT_EQUAL(T0, cfg.ipChecked);
    TEST_ASSERT_TRUE(logHas(logger, "IP publico: 1.2.3.4"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("https://api4.ipify.org", MockHTTP::calledURLs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("ESP8266", MockHTTP::requests[0].headers[0].second.c_str());
    TEST_ASSERT_TRUE(cloudflareTrust(MockTLS::lastTrust));
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
    // IP novo: pergunta o reverso ao DNS da rede
    TEST_ASSERT_EQUAL(1, MockUDP::live);
    TEST_ASSERT_EQUAL(53, MockUDP::port);
}

// Sem DNS nem ntfy configurados: a consulta é do próprio aparelho
void test_consulta_a_cada_5_min_e_so_loga_mudanca() {
    Fix f;
    TEST_ASSERT_TRUE(f.resolver.due(0));
    MockHTTP::push(200, "1.2.3.4");
    f.resolver.tick(T0);
    TEST_ASSERT_FALSE(f.resolver.due(T0 + CHECK - 1));
    f.resolver.tick(T0 + CHECK - 1);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());

    // Mesmo IP: nada no log, só a hora da verificação
    uint8_t lines = logger.count();
    MockHTTP::push(200, "1.2.3.4");
    TEST_ASSERT_TRUE(f.resolver.due(T0 + CHECK));
    f.resolver.tick(T0 + CHECK);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(lines, logger.count());
    TEST_ASSERT_EQUAL(T0, cfg.ipChanged);
    TEST_ASSERT_EQUAL(T0 + CHECK, cfg.ipChecked);

    // Mudou: log com o anterior e o novo, reverso de novo
    strlcpy(cfg.publicHost, "antigo.example", sizeof(cfg.publicHost));
    MockHTTP::push(200, "5.6.7.8");
    f.resolver.tick(T0 + 2 * CHECK);
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", cfg.publicIP);
    TEST_ASSERT_TRUE(logHas(logger, "IP publico mudou: 1.2.3.4 -> 5.6.7.8"));
    TEST_ASSERT_EQUAL(T0 + 2 * CHECK, cfg.ipChanged);
    TEST_ASSERT_EQUAL_STRING("", cfg.publicHost);
    TEST_ASSERT_EQUAL(1, MockUDP::live);
}

void test_sem_wifi_ou_relogio_nao_consulta() {
    Fix f;
    WiFi._status = WL_DISCONNECTED;
    TEST_ASSERT_FALSE(f.resolver.due(T0));
    f.resolver.tick(T0);
    WiFi._status = WL_CONNECTED;
    fakeNow = 1000;
    TEST_ASSERT_FALSE(f.resolver.due(T0));
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(0, logger.count());
}

// Falha vai ao log só na primeira vez; tenta de novo a cada minuto
void test_falha_loga_uma_vez_e_tenta_a_cada_minuto() {
    Fix f;
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(500);
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(NUM_IP_SERVICES, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("", cfg.publicIP);
    TEST_ASSERT_EQUAL(1, logCount("nenhum servico respondeu"));
    TEST_ASSERT_FALSE(f.resolver.due(T0 + RETRY - 1));
    TEST_ASSERT_TRUE(f.resolver.due(T0 + RETRY));

    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(-1);
    f.resolver.tick(T0 + RETRY);
    TEST_ASSERT_EQUAL(1, logCount("nenhum servico respondeu"));

    // Voltou: intervalo normal, e uma falha nova vai ao log de novo
    MockHTTP::push(200, "1.2.3.4");
    f.resolver.tick(T0 + 2 * RETRY);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);
    TEST_ASSERT_FALSE(f.resolver.due(T0 + 2 * RETRY + RETRY));
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(500);
    f.resolver.tick(T0 + 2 * RETRY + CHECK);
    TEST_ASSERT_EQUAL(2, logCount("nenhum servico respondeu"));
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);   // mantém o último conhecido
}

void test_memoria_insuficiente_nao_consulta() {
    Fix f;
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "IP publico: memoria insuficiente"));

    ESP._heap = 50000;
    ESP._maxBlock = PublicIPResolver::MIN_TLS_BLOCK - 1;
    f.resolver.tick(T0 + RETRY);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(1, logCount("memoria insuficiente"));
}

void test_tls_indisponivel() {
    Fix f;
    MockTLS::nullOwned = true;
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "nenhum servico respondeu"));
}

void test_aceita_espacos_e_quebra_de_linha() {
    Fix f;
    MockHTTP::push(200, "  5.6.7.8\n");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", cfg.publicIP);
}

void test_falha_de_conexao_tenta_proximo() {
    Fix f;
    MockHTTP::failNextBegin();
    MockHTTP::push(200, "9.9.9.9");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL_STRING("9.9.9.9", cfg.publicIP);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("https://icanhazip.com", MockHTTP::calledURLs[1].c_str());
}

void test_erros_tentam_o_proximo() {
    Fix f;
    MockHTTP::push(503);
    MockHTTP::push(200, "<html>erro</html>");
    MockHTTP::push(200, "10.0.0.1");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", cfg.publicIP);
    TEST_ASSERT_EQUAL_STRING("https://ifconfig.co/ip", MockHTTP::calledURLs[2].c_str());
    TEST_ASSERT_FALSE(logHas(logger, "nenhum servico"));
}

void test_reverso_sem_resposta_desiste() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.resolver.tick(T0);
    TEST_ASSERT_EQUAL(1, MockUDP::live);
    f.resolver.tick(T0 + ReverseDNS::TIMEOUT - 1);
    TEST_ASSERT_EQUAL(1, MockUDP::live);
    f.resolver.tick(T0 + ReverseDNS::TIMEOUT);
    TEST_ASSERT_EQUAL(0, MockUDP::live);
    TEST_ASSERT_EQUAL_STRING("", cfg.publicHost);
}

// O CloudflareCA.h gerado: cada raiz é um certificado DER completo
// (SEQUENCE com tamanho longo), e os tamanhos cobrem o bloco todo
void test_raizes_der_inteiras() {
    size_t off = 0;
    for (uint16_t len : CLOUDFLARE_LEN) {
        const uint8_t* d = CLOUDFLARE_DER + off;
        TEST_ASSERT_EQUAL_HEX8(0x30, d[0]);
        TEST_ASSERT_EQUAL_HEX8(0x82, d[1]);
        TEST_ASSERT_EQUAL(len, ((d[2] << 8) | d[3]) + 4);
        off += len;
    }
    TEST_ASSERT_EQUAL(sizeof(CLOUDFLARE_DER), off);
    TEST_ASSERT_EQUAL(4, (int)(sizeof(CLOUDFLARE_LEN) / sizeof(CLOUDFLARE_LEN[0])));

    BearSSL::X509List list;
    cloudflareRoots(list);
    TEST_ASSERT_TRUE(cloudflareTrust(list.trust));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_primeiro_servico_ok);
    RUN_TEST(test_consulta_a_cada_5_min_e_so_loga_mudanca);
    RUN_TEST(test_sem_wifi_ou_relogio_nao_consulta);
    RUN_TEST(test_falha_loga_uma_vez_e_tenta_a_cada_minuto);
    RUN_TEST(test_memoria_insuficiente_nao_consulta);
    RUN_TEST(test_tls_indisponivel);
    RUN_TEST(test_aceita_espacos_e_quebra_de_linha);
    RUN_TEST(test_falha_de_conexao_tenta_proximo);
    RUN_TEST(test_erros_tentam_o_proximo);
    RUN_TEST(test_reverso_sem_resposta_desiste);
    RUN_TEST(test_raizes_der_inteiras);
    return UNITY_END();
}
