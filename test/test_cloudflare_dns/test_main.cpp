// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "Clock.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"
#include "CloudflareDNS.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

static const char* OK_JSON  = R"({"success":true,"errors":[],"result":{"id":"x","content":"1.2.3.4"}})";
static const char* ERR_JSON = R"({"success":false,"errors":[{"code":10000,"message":"Authentication error"}]})";

void setUp() {
    resetMocks();
    logger = Logger();
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    cfg = Config{};
    strlcpy(cfg.cf_token, "TOKEN", sizeof(cfg.cf_token));
    strlcpy(cfg.cf_zone,  "ZONE",  sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_records[0], "REC0", sizeof(cfg.cf_records[0]));
    strlcpy(cfg.cf_records[1], "REC1", sizeof(cfg.cf_records[1]));
    cfg.cf_record_count = 2;
}
void tearDown() {}

struct Fixture {
    Clock            clock{fakeTime};
    WiFiService      wifi{cfg, logger};
    PublicIPResolver resolver{logger, clock};
    CloudflareDNS    dns{cfg, logger, wifi, resolver, clock};
};

static const unsigned long T = CloudflareDNS::CHECK_INTERVAL;

void test_configured_exige_token_zona_e_records() {
    Fixture f;
    TEST_ASSERT_TRUE(f.dns.configured());
    cfg.cf_record_count = 0;
    TEST_ASSERT_FALSE(f.dns.configured());
    cfg.cf_record_count = 2;
    cfg.cf_zone[0] = '\0';
    TEST_ASSERT_FALSE(f.dns.configured());
    strlcpy(cfg.cf_zone, "ZONE", sizeof(cfg.cf_zone));
    cfg.cf_token[0] = '\0';
    TEST_ASSERT_FALSE(f.dns.configured());

    f.dns.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_aguarda_intervalo() {
    Fixture f;
    f.dns.tick(T - 1);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_sem_wifi_nao_verifica() {
    Fixture f;
    WiFi._status = WL_DISCONNECTED;
    f.dns.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_sem_relogio_nao_verifica() {
    Fixture f;
    fakeNow = 1000;
    f.dns.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_falha_ao_obter_ip() {
    Fixture f;
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(500);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Falha ao obter IP publico"));
    TEST_ASSERT_EQUAL_STRING("", cfg.publicIP);
}

void test_atualiza_todos_os_records() {
    Fixture f;
    setMillis(T);
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, OK_JSON);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);

    TEST_ASSERT_EQUAL(3, (int)MockHTTP::requests.size());
    const auto& p0 = MockHTTP::requests[1];
    TEST_ASSERT_EQUAL_STRING("PATCH", p0.method.c_str());
    TEST_ASSERT_EQUAL_STRING("https://api.cloudflare.com/client/v4/zones/ZONE/dns_records/REC0", p0.url.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"content\":\"1.2.3.4\"}", p0.payload.c_str());
    TEST_ASSERT_EQUAL_STRING("Authorization", p0.headers[0].first.c_str());
    TEST_ASSERT_EQUAL_STRING("Bearer TOKEN",  p0.headers[0].second.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[2].url, "/dns_records/REC1"));

    // TLS validado com as raízes embarcadas e relógio atual
    TEST_ASSERT_TRUE(cloudflareTrust(MockTLS::lastTrust));
    TEST_ASSERT_EQUAL((long)fakeNow, (long)MockTLS::lastTime);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);

    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", f.dns.syncedIP());
    TEST_ASSERT_EQUAL(T, cfg.lastDnsUpdate);
    TEST_ASSERT_TRUE(logHas(logger, "DNS atualizado!"));
}

void test_ip_inalterado_nao_envia_patch() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, OK_JSON);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);

    MockHTTP::push(200, "1.2.3.4");
    f.dns.tick(2 * T);
    TEST_ASSERT_EQUAL(4, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("GET", MockHTTP::requests[3].method.c_str());
}

void test_ip_mudou_envia_patch_novamente() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, OK_JSON);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);

    MockHTTP::push(200, "5.6.7.8");
    MockHTTP::push(200, OK_JSON);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(2 * T);
    TEST_ASSERT_EQUAL(6, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", f.dns.syncedIP());
}

void test_erro_da_api_nao_marca_sincronizado() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(403, ERR_JSON);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);

    TEST_ASSERT_TRUE(logHas(logger, "DNS erro 10000: Authentication error"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
    TEST_ASSERT_EQUAL(0, cfg.lastDnsUpdate);

    // Próxima verificação tenta de novo, mesmo com o IP igual
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, OK_JSON);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(2 * T);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", f.dns.syncedIP());
}

void test_erro_sem_detalhes() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(500, R"({"success":false})");
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS erro 0: Erro desconhecido"));
}

void test_resposta_json_invalida() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(502, "<html>Bad gateway</html>");
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS resposta invalida (HTTP 502)"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

void test_resposta_json_truncada() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, "{\"success\":");
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS resposta invalida (HTTP 200)"));
}

// Resposta real do PATCH: registro completo, objetos e listas aninhados,
// vírgulas dentro de texto. Lida inteira, sem filtro.
static const char* FULL_JSON =
    R"({"result":{"id":"023e105f4ecef8ad9ca31a8372d0c353","zone_id":"023e105f4ecef8ad9ca31a8372d0c353",)"
    R"("zone_name":"example.com","name":"casa.example.com","type":"A","content":"1.2.3.4",)"
    R"("proxiable":true,"proxied":false,"ttl":1,"settings":{"ipv4_only":true,"ipv6_only":false},)"
    R"("meta":{"auto_added":false,"managed_by_apps":false,"managed_by_argo_tunnel":false},)"
    R"("comment":"casa, escritorio, [teste] {x}","tags":["a:1","b:2"],)"
    R"("created_on":"2014-01-01T05:20:00.12345Z","modified_on":"2014-01-01T05:20:00.12345Z"},)"
    R"("success":true,"errors":[],"messages":[{"code":1000,"message":"ok, feito"}]})";

void test_resposta_completa_da_api() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, FULL_JSON);
    MockHTTP::push(200, FULL_JSON);
    f.dns.tick(T);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", f.dns.syncedIP());
    TEST_ASSERT_TRUE(logHas(logger, "DNS atualizado!"));
}

void test_mensagem_de_erro_com_virgulas() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(400, R"({"success":false,"errors":[{"code":9005,"message":"Content, for A record, is invalid"}],"messages":[],"result":null})");
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS erro 9005: Content, for A record, is invalid"));
}

// JSON válido, mas sem "success" (proxy, outra API): inválido
void test_resposta_sem_campo_success() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, R"({"result":{"id":"x"},"errors":[]})");
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS resposta invalida (HTTP 200)"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

// Documento do tamanho da resposta: se a heap não tiver o bloco, a
// resposta conta como inválida e a próxima verificação tenta de novo
void test_resposta_sem_memoria_para_o_documento() {
    Fixture f;
    size_t values = 0;
    for (const char* p = OK_JSON; *p; p++) values += *p == ',' || *p == '{' || *p == '[';
    TEST_ASSERT_EQUAL(6, (int)values);
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, OK_JSON);
    MockHTTP::push(200, OK_JSON);
    mock::failMallocSize = JSON_ARRAY_SIZE(values);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS resposta invalida (HTTP 200)"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

// A sessão TLS fecha antes de a resposta ser analisada
void test_sessao_tls_fecha_antes_da_analise() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, OK_JSON);
    MockHTTP::push(200, OK_JSON);
    MockTLS::stopCalls = 0;
    f.dns.tick(T);
    TEST_ASSERT_TRUE(MockTLS::stopCalls >= 2);   // um por registro
}

void test_erro_de_conexao_tls() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(-1);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS erro de conexao: -1"));
}

void test_falha_ao_iniciar_conexao() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::beginResults = {true, false, true};
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: falha ao conectar"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

void test_dns_memoria_insuficiente() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    char ip[16];
    TEST_ASSERT_TRUE(f.resolver.resolve(ip, sizeof(ip), T));
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: memoria insuficiente"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_dns_tls_indisponivel() {
    Fixture f;
    MockHTTP::push(200, "1.2.3.4");
    char ip[16];
    TEST_ASSERT_TRUE(f.resolver.resolve(ip, sizeof(ip), T));
    MockTLS::nullOwned = true;
    f.dns.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "DNS: TLS indisponivel"));
    TEST_ASSERT_EQUAL_STRING("", f.dns.syncedIP());
}

void test_sucesso_zera_falhas_do_wifi() {
    Fixture f;
    for (int i = 0; i < WiFiService::MAX_FAILURES - 1; i++) f.wifi.notifyFailure();
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, OK_JSON);
    MockHTTP::push(200, OK_JSON);
    f.dns.tick(T);
    f.wifi.notifyFailure();
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

// O ntfy solta o TLS quando o próximo tick do DNS vai usar a rede
void test_due_avisa_quando_vai_usar_a_rede() {
    Fixture f;
    TEST_ASSERT_FALSE(f.dns.due(T - 1));
    TEST_ASSERT_TRUE(f.dns.due(T));
    WiFi._status = WL_DISCONNECTED;
    TEST_ASSERT_FALSE(f.dns.due(T));
    WiFi._status = WL_CONNECTED;
    fakeNow = 1000;
    TEST_ASSERT_FALSE(f.dns.due(T));
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    cfg.cf_record_count = 0;
    TEST_ASSERT_FALSE(f.dns.due(T));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_configured_exige_token_zona_e_records);
    RUN_TEST(test_aguarda_intervalo);
    RUN_TEST(test_sem_wifi_nao_verifica);
    RUN_TEST(test_sem_relogio_nao_verifica);
    RUN_TEST(test_falha_ao_obter_ip);
    RUN_TEST(test_atualiza_todos_os_records);
    RUN_TEST(test_ip_inalterado_nao_envia_patch);
    RUN_TEST(test_ip_mudou_envia_patch_novamente);
    RUN_TEST(test_erro_da_api_nao_marca_sincronizado);
    RUN_TEST(test_erro_sem_detalhes);
    RUN_TEST(test_resposta_json_invalida);
    RUN_TEST(test_resposta_json_truncada);
    RUN_TEST(test_resposta_completa_da_api);
    RUN_TEST(test_mensagem_de_erro_com_virgulas);
    RUN_TEST(test_resposta_sem_campo_success);
    RUN_TEST(test_resposta_sem_memoria_para_o_documento);
    RUN_TEST(test_sessao_tls_fecha_antes_da_analise);
    RUN_TEST(test_erro_de_conexao_tls);
    RUN_TEST(test_falha_ao_iniciar_conexao);
    RUN_TEST(test_sucesso_zera_falhas_do_wifi);
    RUN_TEST(test_dns_memoria_insuficiente);
    RUN_TEST(test_dns_tls_indisponivel);
    RUN_TEST(test_due_avisa_quando_vai_usar_a_rede);
    return UNITY_END();
}
