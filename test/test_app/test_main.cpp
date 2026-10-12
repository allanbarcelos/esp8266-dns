#include <unity.h>
#include "../test_support.h"
#include "App.h"

void setUp()    { resetMocks(); }
void tearDown() {}

void test_begin_inicializa_servicos() {
    MockFS::put("/config.json", R"({"ssid":"Rede","pass":"12345678","webpss":"senha-forte"})");
    App app;
    app.begin();

    TEST_ASSERT_EQUAL(115200, Serial.baud);
    TEST_ASSERT_EQUAL_STRING("Rede", app.config.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Rede", WiFi.beginSSID.c_str());
    TEST_ASSERT_NOT_NULL(WiFi.gotIP);
    TEST_ASSERT_EQUAL(1, mock::configTimeCalls);
    TEST_ASSERT_TRUE(ESP8266WebServer::instance()->started);
    TEST_ASSERT_TRUE(logHas(app.logger, "Sistema inicializado dev"));
    TEST_ASSERT_TRUE(logHas(app.logger, "Reset: Power On"));
    TEST_ASSERT_TRUE(logHas(app.logger, "OTA: reserva 400 KB"));
    TEST_ASSERT_TRUE(app.ota.automatic());
}

void test_begin_desliga_ota_automatico() {
    MockFS::put("/config.json", R"({"ssid":"Rede","pass":"12345678","webpss":"senha-forte","ota_auto":false})");
    App app;
    app.begin();
    TEST_ASSERT_FALSE(app.config.ota_auto);
    TEST_ASSERT_FALSE(app.ota.automatic());
}

void test_begin_sem_config_entra_em_modo_ap() {
    App app;
    app.begin();
    TEST_ASSERT_TRUE(app.wifi.inConfigMode());
    TEST_ASSERT_EQUAL(WIFI_AP, WiFi._mode);
}

void test_tick_executa_todos_os_servicos() {
    App app;
    app.begin();
    setMillis(WiFiService::DAILY_RESTART + 1);
    app.tick();
    TEST_ASSERT_EQUAL(1, ESP8266WebServer::instance()->handleCalls);
    TEST_ASSERT_TRUE(app.wifi.restartPending());
}

void test_begin_recuperacao_ota() {
    MockFS::put("/config.json", R"({"ssid":"Rede","pass":"12345678","webpss":"senha-forte"})");
    MockFS::put("/ota.pending", "v129 1");
    App app;
    app.begin();
    TEST_ASSERT_TRUE(app.ota.slim());
    TEST_ASSERT_TRUE(app.ota.busy());
    TEST_ASSERT_TRUE(logHas(app.logger, "OTA: modo recuperacao"));
    TEST_ASSERT_EQUAL_STRING("nenhum", app.iot.sensorText());
}

// Recuperação: só Wi-Fi e OTA rodam; painel, DNS, IoT e ntfy ficam parados
void test_tick_em_recuperacao_so_roda_ota() {
    MockFS::put("/config.json", R"({"ssid":"Rede","pass":"12345678","webpss":"senha-forte"})");
    MockFS::put("/ota.pending", "v129 1");
    App app;
    app.begin();
    MockHTTP::push(200, std::string(512, 'X'), 4096);
    app.tick();
    TEST_ASSERT_TRUE(app.ota.inProgress());
    TEST_ASSERT_EQUAL(0, ESP8266WebServer::instance()->handleCalls);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs[0], "/download/v129/"));
}

void test_tick_pausa_dns_durante_ota() {
    MockFS::put("/config.json", R"({"ssid":"Rede","pass":"12345678","webpss":"senha-forte"})");
    App app;
    app.begin();
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(512, 'X'), 4096);
    app.ota.requestCheck();
    app.tick();
    app.ota.requestInstall();
    app.tick();
    TEST_ASSERT_TRUE(app.ota.inProgress());
    size_t before = MockHTTP::calledURLs.size();
    setMillis(120000);
    app.tick();
    TEST_ASSERT_EQUAL((int)before, (int)MockHTTP::calledURLs.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_inicializa_servicos);
    RUN_TEST(test_begin_desliga_ota_automatico);
    RUN_TEST(test_begin_sem_config_entra_em_modo_ap);
    RUN_TEST(test_tick_executa_todos_os_servicos);
    RUN_TEST(test_begin_recuperacao_ota);
    RUN_TEST(test_tick_em_recuperacao_so_roda_ota);
    RUN_TEST(test_tick_pausa_dns_durante_ota);
    return UNITY_END();
}
