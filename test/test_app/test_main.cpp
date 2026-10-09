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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_inicializa_servicos);
    RUN_TEST(test_begin_sem_config_entra_em_modo_ap);
    RUN_TEST(test_tick_executa_todos_os_servicos);
    return UNITY_END();
}
