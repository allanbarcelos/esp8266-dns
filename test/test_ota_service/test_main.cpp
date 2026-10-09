#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "WiFiService.h"
#include "OTAService.h"

static Logger logger;
static Config cfg;

void setUp()    { resetMocks(); logger = Logger(); cfg = Config{}; }
void tearDown() {}

struct Fixture {
    WiFiService wifi{cfg, logger};
    OTAService  ota{logger, wifi};
};

static const unsigned long T = OTAService::CHECK_INTERVAL;

// Enfileira consulta de versão + download de `size` bytes
static void queueUpdate(const std::string& body, int size = -1) {
    MockHTTP::push(200, R"({"tag_name":"v999","name":"x"})");
    MockHTTP::push(200, body, size);
}

// ── versão ───────────────────────────────────────────────────────────────────

void test_parse_tag() {
    TEST_ASSERT_EQUAL(42, OTAService::parseTag("v42"));
    TEST_ASSERT_EQUAL(0,  OTAService::parseTag("v0"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("42"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("v"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("v4a"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("v1.2"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("dev"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag(nullptr));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("v99999999999"));
}

void test_is_newer() {
    TEST_ASSERT_TRUE (OTAService::isNewer("v2", "v1"));
    TEST_ASSERT_FALSE(OTAService::isNewer("v1", "v1"));
    TEST_ASSERT_FALSE(OTAService::isNewer("v1", "v2"));   // sem downgrade
    TEST_ASSERT_FALSE(OTAService::isNewer("lixo", "v1"));
    TEST_ASSERT_TRUE (OTAService::isNewer("v1", "dev"));  // build local
}

// ── agendamento ──────────────────────────────────────────────────────────────

void test_sem_check_antes_do_intervalo() {
    Fixture f;
    f.ota.tick(0);
    f.ota.tick(T - 1);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_check_apos_intervalo() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs[0], "releases/latest"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_sem_check_apos_deadline() {
    Fixture f;
    f.ota.tick(OTAService::DEADLINE);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_sem_check_com_restart_pendente() {
    Fixture f;
    f.wifi.scheduleRestart(1000);
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_sem_check_sem_wifi() {
    Fixture f;
    WiFi._status = WL_DISCONNECTED;
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

// ── consulta de versão ───────────────────────────────────────────────────────

void test_falha_ao_conectar_na_api() {
    Fixture f;
    MockHTTP::failNextBegin();
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Falha ao verificar versao"));
}

void test_erro_http_na_api() {
    Fixture f;
    MockHTTP::push(403);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Erro HTTP versao: 403"));
}

void test_json_invalido_na_api() {
    Fixture f;
    MockHTTP::push(200, "{nao e json");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Erro JSON versao"));
}

void test_tag_vazia() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"  "})");
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

// ── download ─────────────────────────────────────────────────────────────────

void test_inicia_ota_com_binario_assinado() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);

    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1024, f.ota.contentLength());
    TEST_ASSERT_EQUAL(0, (int)f.ota.bytesWritten());
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/allanbarcelos/esp8266-dns/releases/download/v999/firmware.signed.bin",
        MockHTTP::calledURLs[1].c_str());
    TEST_ASSERT_EQUAL(1024, (int)Update._size);
}

void test_falha_ao_conectar_no_download() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::beginResults = {true, false};
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Falha ao iniciar OTA"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_erro_http_no_download() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(404);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "HTTP OTA erro: 404"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_tamanho_invalido() {
    Fixture f;
    queueUpdate("");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Tamanho firmware invalido"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_sem_espaco() {
    Fixture f;
    Update._beginOk = false;
    queueUpdate(std::string(100, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Sem espaco para OTA"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

// ── streaming ────────────────────────────────────────────────────────────────

void test_stream_completo_agenda_restart() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);

    f.ota.tick(T + 1);
    TEST_ASSERT_EQUAL(512, (int)f.ota.bytesWritten());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: 50%"));
    TEST_ASSERT_TRUE(f.ota.inProgress());

    f.ota.tick(T + 2);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, Update._ends);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: 100%"));
    TEST_ASSERT_TRUE(logHas(logger, "OTA concluido"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_progresso_registrado_a_cada_10_por_cento() {
    Fixture f;
    queueUpdate(std::string(512 * 40, 'X'));
    f.ota.tick(T);
    for (int i = 1; i <= 3; i++) f.ota.tick(T + i); // 3/40 = 7,5%
    TEST_ASSERT_FALSE(logHas(logger, "OTA: 0%"));
    f.ota.tick(T + 4);                               // 10%
    TEST_ASSERT_TRUE(logHas(logger, "OTA: 10%"));
}

void test_assinatura_invalida_nao_reinicia() {
    Fixture f;
    Update._endOk = false;
    queueUpdate(std::string(100, 'X'));
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "Erro OTA: Signature Verification Failed"));
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

void test_erro_de_escrita_aborta() {
    Fixture f;
    Update._shortWrite = true;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, Update._aborts);
    TEST_ASSERT_TRUE(logHas(logger, "Erro de escrita OTA"));
}

void test_wifi_cai_durante_ota() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);
    WiFi._status = WL_DISCONNECTED;
    f.ota.tick(T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, Update._aborts);
    TEST_ASSERT_TRUE(logHas(logger, "Wi-Fi caiu durante OTA"));
}

void test_stream_parado_aborta_por_timeout() {
    Fixture f;
    queueUpdate(std::string(512, 'X'), 4096); // servidor promete 4096, entrega 512
    f.ota.tick(T);
    f.ota.tick(T + 1);                         // consome os 512 bytes
    f.ota.tick(T + 1 + OTAService::STALL_TIMEOUT);
    TEST_ASSERT_TRUE(f.ota.inProgress());      // ainda dentro do limite

    f.ota.tick(T + 2 + OTAService::STALL_TIMEOUT);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, Update._aborts);
    TEST_ASSERT_TRUE(logHas(logger, "OTA sem dados"));
}

void test_sem_novo_check_durante_ota() {
    Fixture f;
    queueUpdate(std::string(512, 'X'), 4096);
    f.ota.tick(T);
    f.ota.tick(T + 1);
    size_t before = MockHTTP::calledURLs.size();
    f.ota.tick(T + 2);
    TEST_ASSERT_EQUAL((int)before, (int)MockHTTP::calledURLs.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_tag);
    RUN_TEST(test_is_newer);
    RUN_TEST(test_sem_check_antes_do_intervalo);
    RUN_TEST(test_check_apos_intervalo);
    RUN_TEST(test_sem_check_apos_deadline);
    RUN_TEST(test_sem_check_com_restart_pendente);
    RUN_TEST(test_sem_check_sem_wifi);
    RUN_TEST(test_falha_ao_conectar_na_api);
    RUN_TEST(test_erro_http_na_api);
    RUN_TEST(test_json_invalido_na_api);
    RUN_TEST(test_tag_vazia);
    RUN_TEST(test_inicia_ota_com_binario_assinado);
    RUN_TEST(test_falha_ao_conectar_no_download);
    RUN_TEST(test_erro_http_no_download);
    RUN_TEST(test_tamanho_invalido);
    RUN_TEST(test_sem_espaco);
    RUN_TEST(test_stream_completo_agenda_restart);
    RUN_TEST(test_progresso_registrado_a_cada_10_por_cento);
    RUN_TEST(test_assinatura_invalida_nao_reinicia);
    RUN_TEST(test_erro_de_escrita_aborta);
    RUN_TEST(test_wifi_cai_durante_ota);
    RUN_TEST(test_stream_parado_aborta_por_timeout);
    RUN_TEST(test_sem_novo_check_durante_ota);
    return UNITY_END();
}
