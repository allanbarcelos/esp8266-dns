#include <unity.h>
#include "Config.h"
#include "Logger.h"
#include "WiFiService.h"
#include "OTAService.h"
#include <cstring>

void setUp()    { MockHTTP::reset(); mock::_millis = 0; WiFi._status = WL_CONNECTED; Update._ok = true; }
void tearDown() {}

// ── Testes ───────────────────────────────────────────────────────────────────

void test_sem_check_antes_do_intervalo() {
    Logger lgr; Config cfg; WiFiService wifi(cfg, lgr); OTAService ota(lgr, wifi);
    MockHTTP::push(200, R"({"tag_name":"dev"})");

    ota.tick(0);
    ota.tick(OTAService::CHECK_INTERVAL - 1);

    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_check_apos_intervalo() {
    Logger lgr; Config cfg; WiFiService wifi(cfg, lgr); OTAService ota(lgr, wifi);
    MockHTTP::push(200, R"({"tag_name":"dev"})"); // versão igual → sem download

    ota.tick(OTAService::CHECK_INTERVAL + 1);

    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_NOT_NULL(std::strstr(MockHTTP::calledURLs[0].c_str(), "releases/latest"));
}

void test_sem_check_apos_deadline() {
    Logger lgr; Config cfg; WiFiService wifi(cfg, lgr); OTAService ota(lgr, wifi);
    MockHTTP::push(200, R"({"tag_name":"dev"})");

    ota.tick(OTAService::DEADLINE + 1);

    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_nao_inicia_ota_se_versao_igual() {
    Logger lgr; Config cfg; WiFiService wifi(cfg, lgr); OTAService ota(lgr, wifi);
    MockHTTP::push(200, R"({"tag_name":"dev"})");

    ota.tick(OTAService::CHECK_INTERVAL + 1);

    TEST_ASSERT_FALSE(ota.inProgress());
}

void test_inicia_ota_se_versao_diferente() {
    Logger lgr; Config cfg; WiFiService wifi(cfg, lgr); OTAService ota(lgr, wifi);

    MockHTTP::push(200, R"({"tag_name":"v999"})");   // version check
    MockHTTP::push(200, std::string(512, 'X'));       // firmware download

    ota.tick(OTAService::CHECK_INTERVAL + 1);

    TEST_ASSERT_TRUE(ota.inProgress());
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_NOT_NULL(std::strstr(MockHTTP::calledURLs[1].c_str(), "v999"));
    TEST_ASSERT_NOT_NULL(std::strstr(MockHTTP::calledURLs[1].c_str(), "firmware.bin"));
}

void test_sem_segundo_check_durante_ota() {
    Logger lgr; Config cfg; WiFiService wifi(cfg, lgr); OTAService ota(lgr, wifi);

    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(512, 'X'));

    ota.tick(OTAService::CHECK_INTERVAL + 1); // inicia OTA
    size_t before = MockHTTP::calledURLs.size();

    MockHTTP::push(200, R"({"tag_name":"v999"})");
    ota.tick(OTAService::CHECK_INTERVAL * 2 + 2); // OTA em progresso → sem nova checagem

    TEST_ASSERT_EQUAL((int)before, (int)MockHTTP::calledURLs.size());
}

// ── Runner ────────────────────────────────────────────────────────────────────
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sem_check_antes_do_intervalo);
    RUN_TEST(test_check_apos_intervalo);
    RUN_TEST(test_sem_check_apos_deadline);
    RUN_TEST(test_nao_inicia_ota_se_versao_igual);
    RUN_TEST(test_inicia_ota_se_versao_diferente);
    RUN_TEST(test_sem_segundo_check_durante_ota);
    return UNITY_END();
}
