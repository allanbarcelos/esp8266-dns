// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "WiFiService.h"

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    cfg.ssid = "Rede";
    cfg.pass = "senha123";
}
void tearDown() {}

using W = WiFiService;

// ── begin() ──────────────────────────────────────────────────────────────────

void test_sem_ssid_inicia_ap_protegido() {
    cfg.ssid = "";
    W wifi(cfg, logger);
    wifi.begin();
    TEST_ASSERT_EQUAL(WIFI_AP, WiFi._mode);
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_EQUAL_STRING(AP_SSID, WiFi.apSSID.c_str());
    TEST_ASSERT_EQUAL_STRING(AP_PASSWORD, WiFi.apPass.c_str());
    TEST_ASSERT_TRUE(std::strlen(AP_PASSWORD) >= 8);
    TEST_ASSERT_TRUE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);
}

void test_conecta_em_modo_estacao() {
    W wifi(cfg, logger);
    wifi.begin();
    TEST_ASSERT_EQUAL(WIFI_STA, WiFi._mode);
    TEST_ASSERT_FALSE(WiFi.persistentFlag);
    TEST_ASSERT_TRUE(WiFi.autoReconnectFlag);
    TEST_ASSERT_EQUAL_STRING("Rede", WiFi.beginSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("senha123", WiFi.beginPass.c_str());
    TEST_ASSERT_FALSE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_FALSE(logHas(logger, "Conectado!")); // o tick acompanha a associação
    wifi.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "Conectado!"));
}

// begin() não espera a associação: o loop segue enquanto o Wi-Fi conecta
void test_begin_nao_bloqueia() {
    WiFi._status = WL_DISCONNECTED;
    W wifi(cfg, logger);
    setMillis(5000);
    wifi.begin();
    TEST_ASSERT_EQUAL(5000UL, millis());
    wifi.tick(5000 + W::CONNECT_TIMEOUT - 1);
    TEST_ASSERT_FALSE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL(WIFI_STA, WiFi._mode);
    WiFi._status = WL_CONNECTED;
    wifi.tick(5000 + W::CONNECT_TIMEOUT);
    TEST_ASSERT_FALSE(wifi.inConfigMode());
    TEST_ASSERT_TRUE(logHas(logger, "Conectado!"));
    // conectado: o tick volta a vigiar a associação normalmente
    WiFi._status = WL_DISCONNECTED;
    wifi.tick(5000 + W::CONNECT_TIMEOUT + W::STATUS_INTERVAL);
    TEST_ASSERT_EQUAL(1, WiFi.disconnectCalls);
}

void test_falha_inicia_ap_e_estacao() {
    WiFi._status = WL_DISCONNECTED;
    W wifi(cfg, logger);
    wifi.begin();
    wifi.tick(W::CONNECT_TIMEOUT);
    TEST_ASSERT_EQUAL(WIFI_AP_STA, WiFi._mode);
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_TRUE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL_STRING(AP_SSID, WiFi.apSSID.c_str());
    TEST_ASSERT_TRUE(logHas(logger, "Falha na conexao"));
}

// ── eventos ──────────────────────────────────────────────────────────────────

void test_got_ip_sai_do_modo_configuracao() {
    WiFi._status = WL_DISCONNECTED;
    W wifi(cfg, logger);
    wifi.registerEvents();
    wifi.begin();
    wifi.tick(W::CONNECT_TIMEOUT);
    TEST_ASSERT_TRUE(wifi.inConfigMode());

    WiFi._status = WL_CONNECTED;
    WiFi.gotIP(WiFiEventStationModeGotIP{});
    TEST_ASSERT_FALSE(wifi.inConfigMode());
    TEST_ASSERT_EQUAL(WIFI_STA, WiFi._mode);
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_TRUE(logHas(logger, "Modo AP desligado"));
}

void test_got_ip_em_modo_estacao_nao_muda_modo() {
    W wifi(cfg, logger);
    wifi.registerEvents();
    wifi.begin();
    WiFi._mode = WIFI_STA;
    WiFi.gotIP(WiFiEventStationModeGotIP{});
    TEST_ASSERT_FALSE(logHas(logger, "Modo AP desligado"));
}

void test_got_ip_zera_falhas() {
    W wifi(cfg, logger);
    wifi.registerEvents();
    for (int i = 0; i < W::MAX_FAILURES - 1; i++) wifi.notifyFailure();
    WiFi.gotIP(WiFiEventStationModeGotIP{});
    wifi.notifyFailure();
    TEST_ASSERT_FALSE(wifi.restartPending());
}

void test_evento_desconectado_registra_log() {
    W wifi(cfg, logger);
    wifi.registerEvents();
    WiFi.disconnected(WiFiEventStationModeDisconnected{});
    TEST_ASSERT_TRUE(logHas(logger, "Wi-Fi desconectado"));
}

// ── tick() ───────────────────────────────────────────────────────────────────

void test_modo_configuracao_nao_derruba_ap() {
    cfg.ssid = "";
    W wifi(cfg, logger);
    wifi.begin();
    WiFi._status = WL_DISCONNECTED;

    for (unsigned long t = 0; t <= 10 * W::STATUS_INTERVAL; t += 500) {
        wifi.tick(t);
        wifi.tickRestart(t);
    }
    TEST_ASSERT_EQUAL(WIFI_AP, WiFi._mode);
    TEST_ASSERT_EQUAL(0, WiFi.disconnectCalls);
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);
    TEST_ASSERT_FALSE(wifi.restartPending());
}

void test_reconecta_quando_desconectado() {
    W wifi(cfg, logger);
    WiFi._status = WL_DISCONNECTED;

    wifi.tick(W::STATUS_INTERVAL);
    TEST_ASSERT_EQUAL(1, WiFi.disconnectCalls);
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);

    wifi.tick(W::STATUS_INTERVAL + W::RECONNECT_DELAY - 1);
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);

    wifi.tick(W::STATUS_INTERVAL + W::RECONNECT_DELAY);
    TEST_ASSERT_EQUAL(1, WiFi.beginCalls);
    TEST_ASSERT_EQUAL(WIFI_STA, WiFi._mode);
    TEST_ASSERT_EQUAL(WIFI_NONE_SLEEP, WiFi.sleepMode);
    TEST_ASSERT_TRUE(logHas(logger, "Tentando reconectar"));
}

void test_nao_inicia_reconexao_duplicada() {
    W wifi(cfg, logger);
    WiFi._status = WL_DISCONNECTED;
    wifi.tick(W::STATUS_INTERVAL);        // agenda reconexão (ainda pendente)
    wifi.tick(2 * W::STATUS_INTERVAL);    // nova checagem com reconexão pendente
    TEST_ASSERT_EQUAL(1, WiFi.disconnectCalls);
    TEST_ASSERT_EQUAL(1, WiFi.beginCalls);
}

void test_desconexao_prolongada_agenda_restart() {
    W wifi(cfg, logger);
    WiFi._status = WL_DISCONNECTED;
    unsigned long t = 0;
    for (int i = 0; i < W::MAX_FAILURES; i++) {
        t += W::STATUS_INTERVAL;
        wifi.tick(t);
        wifi.tick(t + W::RECONNECT_DELAY);
    }
    TEST_ASSERT_TRUE(wifi.restartPending());
}

void test_conectado_nao_sonda_nem_reinicia() {
    W wifi(cfg, logger);
    unsigned long t = 0;
    for (int i = 0; i < W::MAX_FAILURES; i++) {
        t += W::STATUS_INTERVAL;
        wifi.tick(t);
    }
    TEST_ASSERT_EQUAL(0, WiFi.disconnectCalls);
    TEST_ASSERT_EQUAL(0, WiFi.beginCalls);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(wifi.restartPending());
}

// ── tickRestart() ────────────────────────────────────────────────────────────

void test_memoria_critica_exige_checagens_consecutivas() {
    W wifi(cfg, logger);
    ESP._heap = 1000;
    unsigned long t = 0;
    for (int i = 0; i < W::MEM_STRIKES - 1; i++) {
        t += W::MEM_CHECK_INTERVAL;
        wifi.tickRestart(t);
    }
    TEST_ASSERT_FALSE(wifi.restartPending());

    // Recupera: zera o contador
    ESP._heap = 50000;
    t += W::MEM_CHECK_INTERVAL;
    wifi.tickRestart(t);

    ESP._frag = 90;
    for (int i = 0; i < W::MEM_STRIKES - 1; i++) {
        t += W::MEM_CHECK_INTERVAL;
        wifi.tickRestart(t);
    }
    TEST_ASSERT_FALSE(wifi.restartPending());

    t += W::MEM_CHECK_INTERVAL;
    wifi.tickRestart(t);
    TEST_ASSERT_TRUE(wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "Memoria critica"));
}

void test_memoria_verificada_apenas_no_intervalo() {
    W wifi(cfg, logger);
    ESP._heap = 1000;
    for (int i = 0; i < 10; i++) wifi.tickRestart(W::MEM_CHECK_INTERVAL);
    TEST_ASSERT_FALSE(wifi.restartPending());
}

void test_ota_inibe_watchdogs() {
    W wifi(cfg, logger);
    wifi.setOTAInProgress(true);
    ESP._heap = 1000;
    for (int i = 1; i <= 10; i++) wifi.tickRestart(i * W::MEM_CHECK_INTERVAL);
    wifi.tickRestart(W::DAILY_RESTART + 1);
    TEST_ASSERT_FALSE(wifi.restartPending());
}

void test_reinicio_diario() {
    W wifi(cfg, logger);
    wifi.tickRestart(W::DAILY_RESTART + 1);
    TEST_ASSERT_TRUE(wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "Reinicio diario"));
}

void test_restart_executa_apos_atraso() {
    W wifi(cfg, logger);
    setMillis(1000);
    wifi.scheduleRestart(500);
    wifi.tickRestart(1499);
    TEST_ASSERT_EQUAL(0, ESP._restarts);
    wifi.tickRestart(1500);
    TEST_ASSERT_EQUAL(1, ESP._restarts);
}

void test_schedule_restart_ignora_segundo_pedido() {
    W wifi(cfg, logger);
    setMillis(0);
    wifi.scheduleRestart(100);
    wifi.scheduleRestart(100000);
    wifi.tickRestart(100);
    TEST_ASSERT_EQUAL(1, ESP._restarts);
}

void test_is_connected_reflete_status() {
    W wifi(cfg, logger);
    TEST_ASSERT_TRUE(wifi.isConnected());
    WiFi._status = WL_DISCONNECTED;
    TEST_ASSERT_FALSE(wifi.isConnected());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sem_ssid_inicia_ap_protegido);
    RUN_TEST(test_conecta_em_modo_estacao);
    RUN_TEST(test_begin_nao_bloqueia);
    RUN_TEST(test_falha_inicia_ap_e_estacao);
    RUN_TEST(test_got_ip_sai_do_modo_configuracao);
    RUN_TEST(test_got_ip_em_modo_estacao_nao_muda_modo);
    RUN_TEST(test_got_ip_zera_falhas);
    RUN_TEST(test_evento_desconectado_registra_log);
    RUN_TEST(test_modo_configuracao_nao_derruba_ap);
    RUN_TEST(test_reconecta_quando_desconectado);
    RUN_TEST(test_nao_inicia_reconexao_duplicada);
    RUN_TEST(test_desconexao_prolongada_agenda_restart);
    RUN_TEST(test_conectado_nao_sonda_nem_reinicia);
    RUN_TEST(test_memoria_critica_exige_checagens_consecutivas);
    RUN_TEST(test_memoria_verificada_apenas_no_intervalo);
    RUN_TEST(test_ota_inibe_watchdogs);
    RUN_TEST(test_reinicio_diario);
    RUN_TEST(test_restart_executa_apos_atraso);
    RUN_TEST(test_schedule_restart_ignora_segundo_pedido);
    RUN_TEST(test_is_connected_reflete_status);
    return UNITY_END();
}
