// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

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
    f.ota.tick(OTAService::FIRST_CHECK - 1);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_primeiro_check_automatico() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.tick(OTAService::FIRST_CHECK);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs[0], "releases/latest"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
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
    MockTLS::failConnect = true;
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

void test_inicia_ota_com_o_arquivo_do_build() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);

    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1024, f.ota.contentLength());
    TEST_ASSERT_EQUAL(0, (int)f.ota.bytesWritten());
    std::string url = "https://github.com/allanbarcelos/esp8266-dns/releases/download/v999/";
    url += OTAService::assetName(OTAService::signingEnabled());
    TEST_ASSERT_EQUAL_STRING(url.c_str(), MockHTTP::calledURLs[1].c_str());
    TEST_ASSERT_EQUAL(1024, (int)Update._size);
}

// Gzip de 32 bytes 0xE9 0x00…, mtime 0. Os 4 últimos bytes são o tamanho
// descomprimido que o eboot vai copiar.
static const uint8_t kGzip[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x7b, 0xc9,
    0x80, 0x1f, 0x00, 0x00, 0x6d, 0x75, 0xd8, 0xae, 0x20, 0x00, 0x00, 0x00
};

static uint32_t ebootSize(const uint8_t* image, size_t n) {
    TEST_ASSERT_TRUE(n >= 6);
    TEST_ASSERT_EQUAL_HEX8(0x1f, image[0]);
    TEST_ASSERT_EQUAL_HEX8(0x8b, image[1]);
    return (uint32_t)image[n - 4]
         | ((uint32_t)image[n - 3] << 8)
         | ((uint32_t)image[n - 2] << 16)
         | ((uint32_t)image[n - 1] << 24);
}

// O mesmo corte de Update.end() quando a verificação está ligada:
// u32 no final = tamanho da assinatura, que não entra na imagem de boot.
static bool stripSignature(const uint8_t* image, size_t n, size_t& payload) {
    if (n < 4) return false;
    uint32_t sigLen = (uint32_t)image[n - 4]
                    | ((uint32_t)image[n - 3] << 8)
                    | ((uint32_t)image[n - 2] << 16)
                    | ((uint32_t)image[n - 1] << 24);
    if (sigLen == 0 || sigLen > n - 4) return false;
    payload = n - 4 - sigLen;
    return payload >= 6;
}

void test_arquivo_bate_com_o_eboot() {
    TEST_ASSERT_EQUAL_STRING("firmware.signed.bin", OTAService::assetName(true));
    TEST_ASSERT_EQUAL_STRING("firmware.bin", OTAService::assetName(false));
    TEST_ASSERT_EQUAL(32, (int)ebootSize(kGzip, sizeof(kGzip)));

    const uint8_t sig[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t signedImg[sizeof(kGzip) + sizeof(sig) + 4];
    std::memcpy(signedImg, kGzip, sizeof(kGzip));
    std::memcpy(signedImg + sizeof(kGzip), sig, sizeof(sig));
    signedImg[sizeof(kGzip) + sizeof(sig) + 0] = (uint8_t)sizeof(sig);
    signedImg[sizeof(kGzip) + sizeof(sig) + 1] = 0;
    signedImg[sizeof(kGzip) + sizeof(sig) + 2] = 0;
    signedImg[sizeof(kGzip) + sizeof(sig) + 3] = 0;

    // Sem remover o trailer, o eboot copiaria 8 bytes, não o firmware.
    TEST_ASSERT_EQUAL((int)sizeof(sig), (int)ebootSize(signedImg, sizeof(signedImg)));

    size_t payload = 0;
    TEST_ASSERT_TRUE(stripSignature(signedImg, sizeof(signedImg), payload));
    TEST_ASSERT_EQUAL(sizeof(kGzip), payload);
    TEST_ASSERT_EQUAL(32, (int)ebootSize(signedImg, payload));

    size_t ignored = 0;
    uint8_t curto[] = {1, 2, 3};
    TEST_ASSERT_FALSE(stripSignature(curto, sizeof(curto), ignored));
    uint8_t semAssinatura[] = {0, 0, 0, 0};
    TEST_ASSERT_FALSE(stripSignature(semAssinatura, sizeof(semAssinatura), ignored));
    uint8_t grande[] = {0x1f, 0x8b, 0, 0, 5, 0, 0, 0};
    TEST_ASSERT_FALSE(stripSignature(grande, sizeof(grande), ignored));

    const char* asset = OTAService::assetName(OTAService::signingEnabled());
    if (OTAService::signingEnabled()) {
        TEST_ASSERT_EQUAL_STRING("firmware.signed.bin", asset);
        TEST_ASSERT_EQUAL(32, (int)ebootSize(signedImg, payload));
    } else {
        TEST_ASSERT_EQUAL_STRING("firmware.bin", asset);
        TEST_ASSERT_EQUAL(32, (int)ebootSize(kGzip, sizeof(kGzip)));
    }
}

void test_falha_ao_conectar_no_download() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockTLS::failOnConnect = 2;
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
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_FALSE(f.wifi.restartPending());
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

void test_automatico_desligado_nao_consulta() {
    Fixture f;
    f.ota.setAutomatic(false);
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.automatic());
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_verificacao_manual_nao_instala() {
    Fixture f;
    f.ota.setAutomatic(false);
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    TEST_ASSERT_EQUAL(OTAService::Report::Checking, f.ota.report());
    f.ota.requestCheck(); // já pendente: não reinicia o pedido
    f.ota.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.ota.pending());
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
    TEST_ASSERT_EQUAL(OTAService::Report::Available, f.ota.report());
    TEST_ASSERT_TRUE(logHas(logger, "Nova versao encontrada: v999"));
}

void test_verificacao_manual_ja_atualizado() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    TEST_ASSERT_EQUAL(OTAService::Report::UpToDate, f.ota.report());
    TEST_ASSERT_EQUAL_STRING("", f.ota.availableVersion());
    TEST_ASSERT_EQUAL_STRING("Firmware ja esta atualizado", f.ota.detail());
}

void test_instalacao_manual() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.requestInstall();
    TEST_ASSERT_TRUE(f.ota.pending());
    f.ota.tick(1);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(128, f.ota.contentLength());
    TEST_ASSERT_TRUE(contains(
        MockHTTP::calledURLs.back(),
        OTAService::assetName(OTAService::signingEnabled())));
}

void test_instalacao_sem_versao_e_ignorada() {
    Fixture f;
    f.ota.setAutomatic(false);
    f.ota.requestInstall();
    TEST_ASSERT_FALSE(f.ota.pending());
    TEST_ASSERT_EQUAL(OTAService::Report::None, f.ota.report());
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_pedidos_ignorados_durante_download() {
    Fixture f;
    Update._endOk = false;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    f.ota.requestCheck();
    f.ota.requestInstall();
    f.ota.tick(T + 1);
    f.ota.tick(T + 2);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.wifi.restartPending());
    int calls = (int)MockHTTP::calledURLs.size();
    f.ota.tick(T + 3);
    TEST_ASSERT_EQUAL(calls, (int)MockHTTP::calledURLs.size());
}

void test_wifi_cai_com_pedido_pendente() {
    Fixture f;
    f.ota.requestCheck();
    WiFi._status = WL_DISCONNECTED;
    f.ota.tick(0);
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());

    WiFi._status = WL_CONNECTED;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(1);
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
    f.ota.requestInstall();
    WiFi._status = WL_DISCONNECTED;
    f.ota.tick(2);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_consulta_memoria_insuficiente() {
    Fixture f;
    ESP._heap = OTAService::GITHUB_HEAP - 1;
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: memoria insuficiente"));
    TEST_ASSERT_TRUE(logHas(logger, "OTA heap"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(LittleFS.exists("/ota.pending"));
}

void test_consulta_bloco_insuficiente() {
    Fixture f;
    ESP._maxBlock = OTAService::GITHUB_BLOCK - 1;
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: memoria insuficiente"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_consulta_tls_indisponivel() {
    MockTLS::nullOwned = true;
    Fixture f;
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("Nova tentativa em 10 s", f.ota.detail());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: TLS indisponivel"));
}

void test_download_memoria_insuficiente() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    ESP._heap = OTAService::GITHUB_HEAP - 1;
    f.ota.requestInstall();
    f.ota.tick(1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: memoria insuficiente"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(LittleFS.exists("/ota.pending"));
}

void test_download_tls_indisponivel() {
    Fixture f;
    MockTLS::failOnConnect = 2;
    queueUpdate(std::string(64, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
    TEST_ASSERT_EQUAL_STRING("Nova tentativa em 10 s", f.ota.detail());
    TEST_ASSERT_TRUE(logHas(logger, "Nova versao encontrada"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
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

void test_sucesso_apaga_pedido() {
    MockFS::put("/ota.pending", "v999 1");
    Fixture f;
    f.ota.begin();
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.tick(0);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    f.ota.tick(1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "OTA concluido"));
}

void test_github_usa_buffer_tls_grande() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(16384, MockTLS::bufIn);
    TEST_ASSERT_EQUAL(512, MockTLS::bufOut);
    TEST_ASSERT_TRUE(MockHTTP::lastHTTP10);
    TEST_ASSERT_EQUAL_STRING("ESP8266-dns", MockHTTP::lastUA.c_str());
}

void test_retenta_depois_do_backoff() {
    Fixture f;
    MockHTTP::push(403);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    f.ota.tick(T + OTAService::RETRY_MS - 1);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(T + OTAService::RETRY_MS);
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_tamanho_desconhecido_completa_no_fim_do_fluxo() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(600, 'Y'), -2);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(0, f.ota.progressPercent());
    f.ota.tick(T + 1);
    f.ota.tick(T + 2);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    f.ota.tick(T + 3);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA concluido"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_tamanho_desconhecido_espera_conexao() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(512, 'Y'), -2);
    MockHTTP::keepOpen = true;
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(512, (int)f.ota.bytesWritten());
    f.ota.tick(T + 2);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    MockHTTP::keepOpen = false;
    f.ota.tick(T + 3);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA concluido"));
}

void test_tamanho_desconhecido_vazio_aborta() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, "", -2);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    f.ota.tick(T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA vazio"));
    TEST_ASSERT_TRUE(f.ota.pending());
}

void test_carrega_pedido_consulta() {
    MockFS::put("/ota.pending", "check 1");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_TRUE(f.ota.slim());
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_EQUAL_STRING("", f.ota.availableVersion());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: retomando consulta"));
    TEST_ASSERT_EQUAL_STRING("check 2", MockFS::files["/ota.pending"].c_str());
}

void test_recuperacao_consulta_instala() {
    MockFS::put("/ota.pending", "check 1");
    Fixture f;
    f.ota.begin();
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.tick(0);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_recuperacao_atualizado_restaura_painel() {
    MockFS::put("/ota.pending", "check 1");
    Fixture f;
    f.ota.begin();
    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.tick(0);
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "restaurando painel"));
}

void test_carrega_pedido_pendente() {
    MockFS::put("/ota.pending", "v129 1");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_TRUE(f.ota.slim());
    TEST_ASSERT_TRUE(f.ota.busy());
    TEST_ASSERT_EQUAL_STRING("v129", f.ota.availableVersion());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: retomando v129"));
    TEST_ASSERT_EQUAL_STRING("v129 2", MockFS::files["/ota.pending"].c_str());
}

void test_pedido_pendente_invalido() {
    MockFS::put("/ota.pending", "lixo 1");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "pedido de recuperacao invalido"));
}

void test_pedido_pendente_sem_contador() {
    MockFS::put("/ota.pending", "v42");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_TRUE(f.ota.slim());
    TEST_ASSERT_EQUAL_STRING("v42", f.ota.availableVersion());
    TEST_ASSERT_EQUAL_STRING("v42 1", MockFS::files["/ota.pending"].c_str());
}

// Travou (OOM) no último boot permitido sem chegar a _persistAndReboot:
// o boot seguinte tem de desistir, não retomar com o mesmo contador.
void test_travamento_gasta_um_boot() {
    MockFS::put("/ota.pending", "v129 3");
    {
        Fixture crashed;
        crashed.ota.begin();
        TEST_ASSERT_TRUE(crashed.ota.slim());
    }
    logger = Logger();
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "recuperacao esgotada"));
}

void test_imagem_rejeitada_nao_e_baixada_de_novo() {
    Fixture f;
    Update._endOk = false;
    queueUpdate(std::string(100, 'X'));
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_TRUE(logHas(logger, "Erro OTA"));
    size_t calls = MockHTTP::calledURLs.size();

    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(2 * T + 1);
    TEST_ASSERT_EQUAL((int)calls + 1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.ota.pending());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: v999 ja foi rejeitada"));
    TEST_ASSERT_EQUAL_STRING("v999 rejeitada; aguardando outra versao", f.ota.detail());

    // Manual continua possível
    Update._endOk = true;
    MockHTTP::push(200, std::string(64, 'X'));
    f.ota.requestInstall();
    f.ota.tick(2 * T + 2);
    TEST_ASSERT_TRUE(f.ota.inProgress());
}

void test_rejeicao_em_recuperacao_restaura_painel() {
    MockFS::put("/ota.pending", "v129 1");
    Fixture f;
    f.ota.begin();
    Update._endOk = false;
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.tick(0);
    f.ota.tick(1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "recuperacao encerrada, restaurando painel"));
    TEST_ASSERT_EQUAL_STRING("v129", MockFS::files["/ota.rejected"].c_str());
}

// Sem heap no painel a consulta vai para recuperação: a marca precisa
// sobreviver ao reinício, senão o aparelho baixa a mesma imagem de novo.
void test_rejeicao_sobrevive_ao_reinicio() {
    MockFS::put("/ota.rejected", "v999");
    MockFS::put("/ota.pending", "check 1");
    Fixture f;
    f.ota.begin();
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: v999 ja foi rejeitada"));
    TEST_ASSERT_TRUE(logHas(logger, "recuperacao concluida, restaurando painel"));
}

void test_versao_nova_ignora_rejeicao_antiga() {
    MockFS::put("/ota.rejected", "v998");
    Fixture f;
    f.ota.begin();
    queueUpdate(std::string(64, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
}

void test_rejeicao_sem_fs_continua_na_ram() {
    Fixture f;
    Update._endOk = false;
    MockFS::failWriteOpen = true;
    queueUpdate(std::string(100, 'X'));
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: nao gravou versao rejeitada"));
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(2 * T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: v999 ja foi rejeitada"));
}

void test_pedido_pendente_vazio() {
    MockFS::put("/ota.pending", "");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
}

void test_pedido_pendente_esgotado() {
    MockFS::put("/ota.pending", "v129 4");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "recuperacao esgotada"));
}

void test_pedido_pendente_nao_abre() {
    MockFS::put("/ota.pending", "v129 1");
    MockFS::failReadOpen = true;
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
}

void test_esgota_tentativas_e_grava_recuperacao() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    unsigned long t = 1;
    for (uint8_t i = 0; i < OTAService::MAX_TRIES; i++) {
        MockHTTP::push(404);
        if (i == 0) f.ota.requestInstall();
        f.ota.tick(t);
        t += OTAService::RETRY_MAX_MS;
    }
    TEST_ASSERT_TRUE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "recuperacao no proximo boot"));
}

void test_recuperacao_esgota_e_reinicia_painel() {
    MockFS::put("/ota.pending", "v129 3");
    Fixture f;
    f.ota.begin();
    unsigned long t = 1;
    for (uint8_t i = 0; i < OTAService::MAX_TRIES; i++) {
        MockHTTP::push(404);
        f.ota.tick(t);
        t += OTAService::RETRY_MAX_MS;
    }
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "desistiu apos recuperacao"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_FALSE(f.ota.slim());
}

void test_nao_grava_recuperacao_se_fs_falha() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    MockFS::failWriteOpen = true;
    unsigned long t = 1;
    for (uint8_t i = 0; i < OTAService::MAX_TRIES; i++) {
        MockHTTP::push(404);
        if (i == 0) f.ota.requestInstall();
        f.ota.tick(t);
        t += OTAService::RETRY_MAX_MS;
    }
    TEST_ASSERT_TRUE(logHas(logger, "nao gravou pedido de recuperacao"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_tamanho_desconhecido_sem_espaco() {
    Fixture f;
    ESP._freeSketch = 0;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(64, 'Z'), -2);
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "Sem espaco para OTA"));
}

void test_segue_redirect_absoluto() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(302, "", -1, "https://cdn.example:443/fw.bin");
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA redirect 302"));
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "cdn.example"));
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "/fw.bin"));
}

void test_segue_redirect_relativo() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(301, "", -1, "/rel/fw.bin");
    MockHTTP::push(200, std::string(64, 'Y'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "/rel/fw.bin"));
}

void test_redirect_invalido() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(307, "", -1, "http://cdn.example/fw.bin");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: redirect invalido"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_redirect_sem_location() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(308);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: redirect sem Location"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_redirects_esgotados() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    for (int i = 0; i < 5; i++)
        MockHTTP::push(302, "", -1, "https://cdn.example/fw.bin");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: redirecionamentos esgotados"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_headers_incompletos() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\nContent-Length: 4";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: headers incompletos"));
}

void test_status_http_invalido() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "NOTHTTP 200\r\n\r\n";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: headers incompletos"));
}

void test_status_sem_codigo() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "HTTP/1.0\r\n\r\n";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: headers incompletos"));
}

void test_cdn_usa_mfln() {
    Fixture f;
    MockTLS::mfln = true;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(302, "", -1, "https://cdn.example/fw.bin");
    MockHTTP::push(200, std::string(32, 'Z'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(4096, MockTLS::bufIn);
}

void test_redirect_sem_path() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(303, "", -1, "https://cdn.example");
    MockHTTP::push(200, std::string(32, 'Q'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "cdn.example/"));
}

void test_redirect_host_longo() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    std::string loc = "https://" + std::string(80, 'a') + "/fw.bin";
    MockHTTP::push(302, "", -1, loc);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: redirect invalido"));
}

void test_consulta_esgota_sem_gravar_pedido() {
    Fixture f;
    f.ota.setAutomatic(false);
    unsigned long t = 0;
    f.ota.requestCheck();
    MockTLS::failConnect = true;
    for (uint8_t i = 0; i < OTAService::MAX_TRIES; i++) {
        f.ota.tick(t);
        t += OTAService::RETRY_MAX_MS;
        if (i + 1 < OTAService::MAX_TRIES && !f.ota.pending())
            f.ota.requestCheck();
    }
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "tentativas esgotadas"));
}

// ── caminhos de erro do TLS/HTTP cru ─────────────────────────────────────────

void test_consulta_headers_incompletos() {
    Fixture f;
    MockHTTP::rawAny = true;
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\nContent-Type: x";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Erro JSON versao: headers"));
    TEST_ASSERT_EQUAL_STRING("Nova tentativa em 10 s", f.ota.detail());
}

// O BearSSL aloca os buffers e o maior bloco livre já não cabe o RX
void test_bloco_encolhe_apos_buffers_tls() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockTLS::shrinkOnBuffers = 2; // 1 = consulta, 2 = download
    MockTLS::shrinkBlockTo = OTAService::GITHUB_RX + OTAService::TLS_TAIL - 1;
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: TLS indisponivel"));
}

// A espera pela resposta não bloqueia: o tick volta na hora, o painel não
// consegue sobrepor pedidos e o prazo vence num tick posterior.
void test_leitura_nao_bloqueia_e_expira_sem_dados() {
    Fixture f;
    f.ota.setAutomatic(false);
    MockHTTP::push(200, R"({"name":"sem tag")");
    MockHTTP::keepOpen = true;
    f.ota.requestCheck();
    unsigned long before = millis();
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(before, millis());           // nenhum delay
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(logHas(logger, "Erro JSON"));

    f.ota.requestCheck();                          // sessão aberta: ignora
    TEST_ASSERT_EQUAL(OTAService::Report::Checking, f.ota.report());
    f.ota.tick(T + OTAService::READ_TIMEOUT - 1);
    TEST_ASSERT_FALSE(logHas(logger, "Erro JSON"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());

    f.ota.tick(T + OTAService::READ_TIMEOUT);
    TEST_ASSERT_TRUE(logHas(logger, "Erro JSON versao: tag_name"));
    TEST_ASSERT_EQUAL_STRING("Nova tentativa em 10 s", f.ota.detail());
}

// Dados chegando aos poucos: cada tick consome o que veio e continua
void test_resposta_em_partes_entre_ticks() {
    Fixture f;
    queueUpdate(std::string(64, 'X'));
    MockHTTP::keepOpen = true;
    f.ota.tick(T);                       // consulta e cabeçalhos do download
    TEST_ASSERT_TRUE(f.ota.inProgress()); // o mock entrega tudo de uma vez

    setUp();
    Fixture g;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "HTTP/1.0 302 F\r\nLocation: /cdn\r\n";
    MockHTTP::keepOpen = true;           // cabeçalho do redirect incompleto
    g.ota.tick(T);
    TEST_ASSERT_FALSE(g.ota.inProgress());
    TEST_ASSERT_TRUE(g.ota.busy());
    g.ota.requestInstall();              // sessão aberta: ignora
    TEST_ASSERT_EQUAL(OTAService::Report::Available, g.ota.report());
    MockHTTP::keepOpen = false;          // servidor fecha sem a linha vazia
    g.ota.tick(T + 1);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: headers incompletos"));
}

void test_header_sem_dois_pontos_e_ignorado() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\nSemValor\r\nContent-Length: 4\r\n\r\nXXXX";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(4, f.ota.contentLength());
}

void test_tag_com_espacos_no_fim() {
    Fixture f;
    f.ota.setAutomatic(false);
    MockHTTP::push(200, "{\"tag_name\":\"v999\t \"}");
    f.ota.requestCheck();
    f.ota.tick(0);
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_download_sem_memoria_para_o_caminho() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    mock::failMallocSize = OTAService::OTA_PATH_CAP;
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: memoria insuficiente"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_EQUAL_STRING("v999 1", MockFS::files["/ota.pending"].c_str());
}

// ── parser de cabeçalhos do download ─────────────────────────────────────────

// Consulta válida e a resposta crua do primeiro GET do download
static void rawDownload(const std::string& http) {
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = http;
}

void test_status_cortado() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: headers incompletos"));
}

void test_status_com_codigo_invalido() {
    const char* lines[] = {"HTTP/1.0 !00 x\r\n\r\n", "HTTP/1.0 x00 y\r\n\r\n"};
    for (const char* line : lines) {
        setUp();
        Fixture f;
        rawDownload(line);
        f.ota.tick(T);
        TEST_ASSERT_TRUE(logHas(logger, "OTA: headers incompletos"));
    }
}

void test_status_longo_e_truncado() {
    Fixture f;
    rawDownload("HTTP/1.0 200 " + std::string(40, 'O') + "\r\nContent-Length: 4\r\n\r\nXXXX");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(4, f.ota.contentLength());
}

void test_header_cortado_apos_dois_pontos() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK\r\nX:");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: headers incompletos"));
}

void test_header_com_tab_apos_dois_pontos() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK\r\nContent-Length:\t4\r\n\r\nXXXX");
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(4, f.ota.contentLength());
}

void test_nome_de_header_longo() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK\r\n" + std::string(40, 'H') + ": v\r\nContent-Length: 4\r\n\r\nXXXX");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(4, f.ota.contentLength());
}

void test_content_length_longo_e_truncado() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK\r\nContent-Length: " + std::string(40, '0') + "\r\n\r\n");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Tamanho firmware invalido"));
}

void test_location_longo_e_truncado() {
    Fixture f;
    rawDownload("HTTP/1.0 302 F\r\nLocation: /" + std::string(1300, 'a') + "\r\n\r\n");
    MockHTTP::push(200, std::string(32, 'Z'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    std::string expected = "https://github.com/" + std::string(OTAService::OTA_PATH_CAP - 2, 'a');
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), MockHTTP::calledURLs.back().c_str());
}

void test_primeiro_location_vale() {
    Fixture f;
    rawDownload("HTTP/1.0 302 F\r\nLocation: /a\r\nLocation: /b\r\n\r\n");
    MockHTTP::push(200, std::string(32, 'Z'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL_STRING("https://github.com/a", MockHTTP::calledURLs.back().c_str());
}

void test_redirect_sem_host() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(302, "", -1, "https:///fw.bin");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: redirect invalido"));
}

// ── parser de tag_name ───────────────────────────────────────────────────────

static void checkWith(Fixture& f, const std::string& body) {
    f.ota.setAutomatic(false);
    MockHTTP::push(200, body);
    f.ota.requestCheck();
    f.ota.tick(0);
}

void test_tag_apos_aspas_repetidas() {
    Fixture f;
    checkWith(f, R"({"a":"","tag_name":"v999"})");
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_tag_com_espacos_em_volta_dos_dois_pontos() {
    Fixture f;
    checkWith(f, "{\"tag_name\" \t\r\n: \t\r\n\"v999\"}");
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_tag_com_tab_no_inicio() {
    Fixture f;
    checkWith(f, "{\"tag_name\":\"\t v999\"}");
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_tag_longa_e_truncada() {
    Fixture f;
    checkWith(f, R"({"tag_name":"v12345678901234567890"})");
    TEST_ASSERT_FALSE(logHas(logger, "Erro JSON versao"));
    TEST_ASSERT_EQUAL(OTAService::Report::UpToDate, f.ota.report()); // v12345678901234 não é tag válida
}

void test_tag_malformada() {
    const char* bodies[] = {
        R"({"tag_name")",      // termina antes dos dois-pontos
        R"({"tag_name" x)",    // sem dois-pontos
        R"({"tag_name":)",     // termina antes do valor
        R"({"tag_name":5})",   // valor não é string
        R"({"tag_name":"v99)", // string sem fechar
    };
    for (const char* body : bodies) {
        setUp();
        Fixture f;
        checkWith(f, body);
        TEST_ASSERT_TRUE_MESSAGE(logHas(logger, "Erro JSON versao: tag_name"), body);
        TEST_ASSERT_EQUAL_STRING("", f.ota.availableVersion());
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_tag);
    RUN_TEST(test_is_newer);
    RUN_TEST(test_sem_check_antes_do_intervalo);
    RUN_TEST(test_primeiro_check_automatico);
    RUN_TEST(test_check_apos_intervalo);
    RUN_TEST(test_sem_check_apos_deadline);
    RUN_TEST(test_sem_check_com_restart_pendente);
    RUN_TEST(test_sem_check_sem_wifi);
    RUN_TEST(test_falha_ao_conectar_na_api);
    RUN_TEST(test_erro_http_na_api);
    RUN_TEST(test_json_invalido_na_api);
    RUN_TEST(test_tag_vazia);
    RUN_TEST(test_inicia_ota_com_o_arquivo_do_build);
    RUN_TEST(test_arquivo_bate_com_o_eboot);
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
    RUN_TEST(test_automatico_desligado_nao_consulta);
    RUN_TEST(test_verificacao_manual_nao_instala);
    RUN_TEST(test_verificacao_manual_ja_atualizado);
    RUN_TEST(test_instalacao_manual);
    RUN_TEST(test_instalacao_sem_versao_e_ignorada);
    RUN_TEST(test_pedidos_ignorados_durante_download);
    RUN_TEST(test_wifi_cai_com_pedido_pendente);
    RUN_TEST(test_consulta_memoria_insuficiente);
    RUN_TEST(test_consulta_bloco_insuficiente);
    RUN_TEST(test_consulta_tls_indisponivel);
    RUN_TEST(test_download_memoria_insuficiente);
    RUN_TEST(test_download_tls_indisponivel);
    RUN_TEST(test_sucesso_apaga_pedido);
    RUN_TEST(test_github_usa_buffer_tls_grande);
    RUN_TEST(test_retenta_depois_do_backoff);
    RUN_TEST(test_tamanho_desconhecido_completa_no_fim_do_fluxo);
    RUN_TEST(test_tamanho_desconhecido_espera_conexao);
    RUN_TEST(test_tamanho_desconhecido_vazio_aborta);
    RUN_TEST(test_carrega_pedido_consulta);
    RUN_TEST(test_recuperacao_consulta_instala);
    RUN_TEST(test_recuperacao_atualizado_restaura_painel);
    RUN_TEST(test_carrega_pedido_pendente);
    RUN_TEST(test_pedido_pendente_invalido);
    RUN_TEST(test_pedido_pendente_sem_contador);
    RUN_TEST(test_travamento_gasta_um_boot);
    RUN_TEST(test_imagem_rejeitada_nao_e_baixada_de_novo);
    RUN_TEST(test_rejeicao_em_recuperacao_restaura_painel);
    RUN_TEST(test_rejeicao_sobrevive_ao_reinicio);
    RUN_TEST(test_versao_nova_ignora_rejeicao_antiga);
    RUN_TEST(test_rejeicao_sem_fs_continua_na_ram);
    RUN_TEST(test_pedido_pendente_vazio);
    RUN_TEST(test_pedido_pendente_esgotado);
    RUN_TEST(test_pedido_pendente_nao_abre);
    RUN_TEST(test_esgota_tentativas_e_grava_recuperacao);
    RUN_TEST(test_recuperacao_esgota_e_reinicia_painel);
    RUN_TEST(test_nao_grava_recuperacao_se_fs_falha);
    RUN_TEST(test_tamanho_desconhecido_sem_espaco);
    RUN_TEST(test_consulta_esgota_sem_gravar_pedido);
    RUN_TEST(test_segue_redirect_absoluto);
    RUN_TEST(test_segue_redirect_relativo);
    RUN_TEST(test_redirect_invalido);
    RUN_TEST(test_redirect_sem_location);
    RUN_TEST(test_redirects_esgotados);
    RUN_TEST(test_headers_incompletos);
    RUN_TEST(test_status_http_invalido);
    RUN_TEST(test_status_sem_codigo);
    RUN_TEST(test_cdn_usa_mfln);
    RUN_TEST(test_redirect_sem_path);
    RUN_TEST(test_redirect_host_longo);
    RUN_TEST(test_consulta_headers_incompletos);
    RUN_TEST(test_bloco_encolhe_apos_buffers_tls);
    RUN_TEST(test_leitura_nao_bloqueia_e_expira_sem_dados);
    RUN_TEST(test_resposta_em_partes_entre_ticks);
    RUN_TEST(test_header_sem_dois_pontos_e_ignorado);
    RUN_TEST(test_tag_com_espacos_no_fim);
    RUN_TEST(test_download_sem_memoria_para_o_caminho);
    RUN_TEST(test_status_cortado);
    RUN_TEST(test_status_com_codigo_invalido);
    RUN_TEST(test_status_longo_e_truncado);
    RUN_TEST(test_header_cortado_apos_dois_pontos);
    RUN_TEST(test_header_com_tab_apos_dois_pontos);
    RUN_TEST(test_nome_de_header_longo);
    RUN_TEST(test_content_length_longo_e_truncado);
    RUN_TEST(test_location_longo_e_truncado);
    RUN_TEST(test_primeiro_location_vale);
    RUN_TEST(test_redirect_sem_host);
    RUN_TEST(test_tag_apos_aspas_repetidas);
    RUN_TEST(test_tag_com_espacos_em_volta_dos_dois_pontos);
    RUN_TEST(test_tag_com_tab_no_inicio);
    RUN_TEST(test_tag_longa_e_truncada);
    RUN_TEST(test_tag_malformada);
    return UNITY_END();
}
