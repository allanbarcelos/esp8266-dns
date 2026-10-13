// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "OTAService.h"
#include "WebService.h"
#include "../web_browser.h"

using Args = std::map<std::string, std::string>;

static Logger logger;
static Config cfg;

struct Fixture {
    ConfigStore store{cfg, logger};
    WiFiService wifi{cfg, logger};
    OTAService  ota{logger, wifi};
    IoTService  iot{cfg, logger};
    Clock            clock;
    NtfyNotifier     ntfy{cfg, logger, clock, &iot};
    WiFiClient       mqttNet;
    MqttService      mqtt{cfg, logger, iot, mqttNet};
    WebService  web;
    MockBrowser srv{[this]() { web.tick(); }};

    explicit Fixture(bool withNtfy = true, bool withMqtt = true)
        : web(cfg, logger, store, wifi, ota, iot, withNtfy ? &ntfy : nullptr,
              withMqtt ? &mqtt : nullptr) { web.begin(); }

    bool get(const char* path)              { return srv.request(HTTP_GET, path); }
    bool post(const char* path, Args a = {}) {
        a["_csrf"] = web.csrfToken();
        return srv.request(HTTP_POST, path, a);
    }
    const std::string& out() { return srv.out; }

    void enterConfigMode() {
        cfg.ssid = "";
        wifi.begin();
    }
    void startOta() {
        MockHTTP::push(200, R"({"tag_name":"v999"})");
        MockHTTP::push(200, std::string(1000, 'X'));
        ota.tick(OTAService::CHECK_INTERVAL);
        ota.tick(OTAService::CHECK_INTERVAL + 1); // 512 de 1000 bytes
    }
};

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    cfg.webusr = "admin";
    cfg.webpss = "senha-forte";
    cfg.ssid   = "Rede";
}
void tearDown() {}

// ── utilitários estáticos ────────────────────────────────────────────────────

void test_escape() {
    TEST_ASSERT_EQUAL_STRING("&lt;a href=&quot;x&quot;&gt;&amp;&#39;ok",
        WebService::escape("<a href=\"x\">&'ok").c_str());
}

void test_mask_secret() {
    TEST_ASSERT_EQUAL_STRING("",         WebService::maskSecret("").c_str());
    TEST_ASSERT_EQUAL_STRING("****abc",  WebService::maskSecret("abc").c_str());
    TEST_ASSERT_EQUAL_STRING("****wxyz", WebService::maskSecret("abcdwxyz").c_str());
}

void test_format_duration() {
    TEST_ASSERT_EQUAL_STRING("1h 01m 01s", WebService::formatDuration(3661000UL).c_str());
}

void test_format_bytes() {
    TEST_ASSERT_EQUAL_STRING("0 B", WebService::formatBytes(0).c_str());
    TEST_ASSERT_EQUAL_STRING("8 B", WebService::formatBytes(8).c_str());
    TEST_ASSERT_EQUAL_STRING("1000 B", WebService::formatBytes(1000).c_str());
    TEST_ASSERT_EQUAL_STRING("1 KB", WebService::formatBytes(1024).c_str());
    TEST_ASSERT_EQUAL_STRING("1.5 KB", WebService::formatBytes(1500).c_str());
    TEST_ASSERT_EQUAL_STRING("1 MB", WebService::formatBytes(1024UL * 1024UL).c_str());
    TEST_ASSERT_EQUAL_STRING("1.5 MB", WebService::formatBytes(1572864).c_str());
    TEST_ASSERT_EQUAL_STRING("1 GB", WebService::formatBytes(1024UL * 1024UL * 1024UL).c_str());
    TEST_ASSERT_EQUAL_STRING("1.5 GB", WebService::formatBytes(1610612736UL).c_str());
}

// ── inicialização e segurança ───────────────────────────────────────────────

void test_begin_abre_a_porta_e_gera_csrf_aleatorio() {
    Fixture f;
    TEST_ASSERT_TRUE(MockServer::begun);
    TEST_ASSERT_TRUE(logHas(logger, "Servidor web iniciado na porta 80"));
    TEST_ASSERT_EQUAL(16, (int)std::strlen(f.web.csrfToken()));

    std::string first = f.web.csrfToken();
    Fixture g;
    TEST_ASSERT_TRUE(first != g.web.csrfToken());
}

void test_tick_atende_clientes() {
    Fixture f;
    f.web.tick();   // ninguém conectado
    TEST_ASSERT_TRUE(f.get("/status"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(f.srv.conn->closed);
}

// Caminho ou método sem rota: 404 vazio
void test_rota_desconhecida() {
    Fixture f;
    TEST_ASSERT_FALSE(f.get("/nada"));
    TEST_ASSERT_EQUAL(404, f.srv.code);
    TEST_ASSERT_FALSE(f.post("/status"));
    TEST_ASSERT_FALSE(f.get("/save"));
    TEST_ASSERT_FALSE(f.srv.send("PUT / HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_EQUAL(404, f.srv.code);
}

void test_sem_senha_redireciona_para_definir_senha() {
    cfg.webpss = "";
    Fixture f;
    const char* paths[] = {"/", "/status", "/log", "/cloudflare", "/ntfy", "/update", "/iot"};
    for (const char* p : paths) {
        f.get(p);
        TEST_ASSERT_EQUAL(302, f.srv.code);
        TEST_ASSERT_EQUAL_STRING("/password", f.srv.headers["Location"].c_str());
    }
    f.post("/save", {{"ssid", "x"}});
    TEST_ASSERT_EQUAL(302, f.srv.code);
}

void test_autenticacao_obrigatoria() {
    Fixture f;
    f.srv.authOk = false;
    const char* paths[] = {"/", "/status", "/log", "/cloudflare", "/ntfy", "/mqtt", "/password", "/update", "/iot"};
    for (const char* p : paths) {
        f.get(p);
        TEST_ASSERT_EQUAL(401, f.srv.code);
        TEST_ASSERT_EQUAL_STRING("Basic realm=\"ESP8266\"", f.srv.headers["WWW-Authenticate"].c_str());
    }
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/mqtt/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install", "/iot/save", "/iot/cmd", "/iot/token", "/iot/clear"};
    for (const char* p : posts) {
        f.post(p);
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
}

// Só Basic, com usuário e senha do painel, comparados por inteiro
void test_autenticacao_basic() {
    Fixture f;
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);

    f.srv.basicPass = "senha-errada";
    f.get("/status");
    TEST_ASSERT_EQUAL(401, f.srv.code);
    f.srv.basicPass = "senha-forte";
    f.srv.basicUser = "outro";
    f.get("/status");
    TEST_ASSERT_EQUAL(401, f.srv.code);

    // admin:senha-forte
    const char* ok = "Basic YWRtaW46c2VuaGEtZm9ydGU=";
    const char* bad[] = {
        "Basic YWRtaW46c2VuaGEtZm9ydGU",              // cortado
        "Basic YWRtaW46c2VuaGEtZm9ydGU==",            // sobrando
        "basic YWRtaW46c2VuaGEtZm9ydGU=",             // outro esquema
        "Digest username=\"admin\", realm=\"ESP8266\"",
        "Bearer YWRtaW46c2VuaGEtZm9ydGU=",
        "",
    };
    f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", ok}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    for (const char* auth : bad) {
        f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", auth}});
        TEST_ASSERT_EQUAL(401, f.srv.code);
        f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", ok}});   // zera a conta de erros
    }
}

void test_csrf_invalido_rejeitado_em_todos_os_posts() {
    Fixture f;
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/mqtt/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install", "/iot/save", "/iot/cmd", "/iot/token", "/iot/clear"};
    for (const char* p : posts) {
        f.srv.request(HTTP_POST, p, {{"_csrf", "00000000deadbeef"}});
        TEST_ASSERT_EQUAL(403, f.srv.code);
    }
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

// ── página inicial ───────────────────────────────────────────────────────────

void test_root_modo_estacao() {
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Test&lt;SSID&gt;"));   // SSID escapado
    TEST_ASSERT_TRUE(contains(f.out(), "IP Publico"));
    TEST_ASSERT_TRUE(contains(f.out(), "nunca"));
    TEST_ASSERT_TRUE(contains(f.out(), "Firmware: dev"));
    TEST_ASSERT_FALSE(contains(f.out(), "OTA em progresso"));
    TEST_ASSERT_TRUE(contains(f.out(), "/esp01.webp"));
    TEST_ASSERT_TRUE(contains(f.out(), "alt='ESP-01'"));
    TEST_ASSERT_TRUE(contains(f.out(), "Firmware para o modulo ESP-01"));
}

void test_root_com_ip_e_ultima_atualizacao() {
    Fixture f;
    strlcpy(cfg.publicIP, "8.8.8.8", sizeof(cfg.publicIP));
    strlcpy(cfg.cf_host, "casa.exemplo.com", sizeof(cfg.cf_host));
    cfg.lastDnsUpdate = 1000;
    setMillis(61000);
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "8.8.8.8"));
    TEST_ASSERT_TRUE(contains(f.out(), "casa.exemplo.com"));
    TEST_ASSERT_TRUE(contains(f.out(), "0h 01m 00s atras"));
}

void test_root_mostra_progresso_ota() {
    Fixture f;
    f.startOta();
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "OTA em progresso"));
    TEST_ASSERT_TRUE(contains(f.out(), "51%"));
    TEST_ASSERT_TRUE(contains(f.out(), "512 B / 1000 B"));
}

void test_root_modo_configuracao_primeiro_acesso() {
    cfg.webpss = "";
    cfg.webusr = "";
    Fixture f;
    f.enterConfigMode();
    f.get("/");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "/esp01.webp"));
    TEST_ASSERT_TRUE(contains(f.out(), "Firmware para o modulo ESP-01"));
    TEST_ASSERT_TRUE(contains(f.out(), "Modo de configuracao"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='pass1'"));      // define senha do painel
    TEST_ASSERT_TRUE(contains(f.out(), "value='admin'"));
    TEST_ASSERT_TRUE(contains(f.out(), f.web.csrfToken()));
    // SSID vazio não pode virar pedaço vazio (terminador do chunked)
    TEST_ASSERT_TRUE(contains(f.out(), "name='ssid' value=''"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='pass'"));
    TEST_ASSERT_TRUE(contains(f.out(), "</html>"));
    // Modo AP não tem internet: o estilo sai do próprio aparelho, nada de fora
    TEST_ASSERT_TRUE(contains(f.out(), "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"));
    TEST_ASSERT_FALSE(contains(f.out(), "<script src"));
    TEST_ASSERT_FALSE(contains(f.out(), "https://"));
}

void test_root_modo_configuracao_preserva_usuario() {
    cfg.webpss = "";
    cfg.webusr = "joao";
    Fixture f;
    f.enterConfigMode();
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "value='joao'"));
}

void test_root_modo_configuracao_com_senha_definida() {
    Fixture f;
    f.enterConfigMode();
    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "name='ssid'"));
    TEST_ASSERT_FALSE(contains(f.out(), "name='pass1'"));
}

// ── /save (Wi-Fi) ────────────────────────────────────────────────────────────

void test_save_wifi_ok() {
    Fixture f;
    f.post("/save", {{"ssid", "NovaRede"}, {"pass", "12345678"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Reiniciando"));
    TEST_ASSERT_EQUAL_STRING("NovaRede", cfg.ssid.c_str());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "NovaRede"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_save_wifi_rede_aberta() {
    Fixture f;
    f.post("/save", {{"ssid", "Aberta"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.pass.c_str());
}

void test_save_wifi_validacoes() {
    Fixture f;
    f.post("/save", {{"ssid", ""}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "SSID invalido"));

    f.post("/save", {{"ssid", std::string(33, 's')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);

    f.post("/save", {{"ssid", "Rede"}, {"pass", "curta"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "8 a 63"));

    f.post("/save", {{"ssid", "Rede"}, {"pass", std::string(64, 'p')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);

    TEST_ASSERT_FALSE(f.wifi.restartPending());
    TEST_ASSERT_EQUAL(0, (int)MockFS::files.size());
}

void test_save_wifi_primeiro_acesso_exige_senha_do_painel() {
    cfg.webpss = "";
    Fixture f;
    f.enterConfigMode();

    f.post("/save", {{"ssid", "Rede"}, {"user", "eu"}, {"pass1", "abcdefgh"}, {"pass2", "outra123"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "nao coincidem"));

    f.post("/save", {{"ssid", "Rede"}, {"user", "eu"}, {"pass1", "curta"}, {"pass2", "curta"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "8 a 64"));

    f.post("/save", {{"ssid", "Rede"}, {"user", "  "}, {"pass1", "abcdefgh"}, {"pass2", "abcdefgh"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Usuario invalido"));
    TEST_ASSERT_EQUAL_STRING("", cfg.webpss.c_str());

    f.post("/save", {{"ssid", "Rede"}, {"user", "eu"}, {"pass1", "abcdefgh"}, {"pass2", "abcdefgh"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("eu",       cfg.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("abcdefgh", cfg.webpss.c_str());
}

void test_save_wifi_falha_ao_gravar() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/save", {{"ssid", "Rede2"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

// ── Imagens embutidas ────────────────────────────────────────────────────────

void test_imagem_do_aparelho_embutida() {
    cfg.webpss = "";   // pública: carrega antes de haver senha
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/esp01.webp"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("image/webp", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL(sizeof(ASSET_PHOTO), f.out().size());
    TEST_ASSERT_EQUAL(0, f.out().compare(0, 4, "RIFF"));
    TEST_ASSERT_EQUAL(0, f.out().compare(8, 4, "WEBP"));
    TEST_ASSERT_EQUAL_STRING("max-age=604800", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL(0u, f.srv.headers.count("Content-Encoding"));
}

// CSS e JS do painel: gzip, públicos (a página de configuração inicial usa)
static void assetGz(const char* path, const char* type, size_t len) {
    cfg.webpss = "";
    Fixture f;
    TEST_ASSERT_TRUE(f.get(path));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING(type, f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("gzip", f.srv.headers["Content-Encoding"].c_str());
    TEST_ASSERT_EQUAL_STRING("max-age=604800", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL(len, f.out().size());
    TEST_ASSERT_EQUAL_HEX8(0x1f, (uint8_t)f.out()[0]);
    TEST_ASSERT_EQUAL_HEX8(0x8b, (uint8_t)f.out()[1]);
}

void test_css_e_js_em_gzip() {
    assetGz("/s.css", "text/css", sizeof(WEB_CSS_GZ));
    assetGz("/iot.js", "text/javascript", sizeof(WEB_IOT_GZ));
    TEST_ASSERT_EQUAL(8, (int)std::strlen(WEB_CSS_TAG));
    TEST_ASSERT_EQUAL(8, (int)std::strlen(WEB_IOT_TAG));
}

void test_pinos_embutidos_em_svg_gzip() {
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/pinout.svg"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("gzip", f.srv.headers["Content-Encoding"].c_str());
    TEST_ASSERT_EQUAL(sizeof(ASSET_PINOUT_GZ), f.out().size());
    TEST_ASSERT_EQUAL_HEX8(0x1f, (uint8_t)f.out()[0]);   // assinatura gzip
    TEST_ASSERT_EQUAL_HEX8(0x8b, (uint8_t)f.out()[1]);
}

// ── /log ─────────────────────────────────────────────────────────────────────

void test_log_escapa_conteudo() {
    Fixture f;
    logger.log("<script>alert(1)</script>");
    f.get("/log");
    TEST_ASSERT_TRUE(contains(f.out(), "&lt;script&gt;"));
    TEST_ASSERT_FALSE(contains(f.out(), "<script>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Servidor web iniciado"));
}

// ── /status ──────────────────────────────────────────────────────────────────

void test_status_conectado() {
    Fixture f;
    MockFS::put("/config.json", "{}");
    ESP._resetInfo = "Fatal <exception>";
    f.get("/status");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "http-equiv='refresh' content='30'"));
    TEST_ASSERT_TRUE(contains(f.out(), "Heap livre</span><b>48.8 KB</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Maior bloco</span><b>39.1 KB</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Flash"));
    TEST_ASSERT_TRUE(contains(f.out(), ">1 MB<"));
    TEST_ASSERT_TRUE(contains(f.out(), ">293 KB<"));
    TEST_ASSERT_TRUE(contains(f.out(), "Reserva OTA"));
    TEST_ASSERT_TRUE(contains(f.out(), ">400 KB<"));
    TEST_ASSERT_TRUE(contains(f.out(), "LittleFS"));
    TEST_ASSERT_TRUE(contains(f.out(), ">64 KB<"));
    TEST_ASSERT_TRUE(contains(f.out(), ">8 KB<"));
    TEST_ASSERT_TRUE(contains(f.out(), "dead"));               // chip id em hex
    TEST_ASSERT_TRUE(contains(f.out(), "192.168.1.1"));
    TEST_ASSERT_TRUE(contains(f.out(), "AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_TRUE(contains(f.out(), "Fatal &lt;exception&gt;"));
    TEST_ASSERT_TRUE(contains(f.out(), "<section class='c'><h2>Memoria</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"));
    TEST_ASSERT_FALSE(contains(f.out(), "Temp"));
}

void test_status_desconectado_e_ota() {
    Fixture f;
    f.startOta();
    WiFi._status = WL_DISCONNECTED;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "OTA em progresso"));
    TEST_ASSERT_FALSE(contains(f.out(), "192.168.1.1"));
}

// IP público consultado pelo aparelho, com o reverso e os horários
void test_status_ip_publico() {
    Fixture f;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>IP Publico</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Reverso</span><b>-</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Ultima verificacao</span><b>nunca</b>"));

    strlcpy(cfg.publicIP, "200.1.2.3", sizeof(cfg.publicIP));
    strlcpy(cfg.publicHost, "200-1-2-3.isp<x>.net", sizeof(cfg.publicHost));
    cfg.ipChanged = 1000;
    cfg.ipChecked = 241000;
    setMillis(301000);
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "IP</span><b>200.1.2.3</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Reverso</span><b>200-1-2-3.isp&lt;x&gt;.net</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Mudou</span><b>0h 05m 00s atras</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Ultima verificacao</span><b>0h 01m 00s atras</b>"));
}

// ── /cloudflare ──────────────────────────────────────────────────────────────

void test_cloudflare_sem_token() {
    Fixture f;
    f.get("/cloudflare");
    TEST_ASSERT_TRUE(contains(f.out(), "Token nao configurado"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='record'"));
    TEST_ASSERT_FALSE(contains(f.out(), "name='record1'"));
}

void test_cloudflare_com_token_mascarado_e_valores_escapados() {
    Fixture f;
    strlcpy(cfg.cf_token, "segredo-abcd", sizeof(cfg.cf_token));
    strlcpy(cfg.cf_zone, "z'1", sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_record, "r<0>", sizeof(cfg.cf_record));
    f.get("/cloudflare");
    TEST_ASSERT_TRUE(contains(f.out(), "****abcd"));
    TEST_ASSERT_FALSE(contains(f.out(), "segredo"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='z&#39;1'"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='r&lt;0&gt;'"));
}

// ── Aparelho: nome e hostname ────────────────────────────────────────────────

void test_nome_no_titulo_e_no_topo() {
    Fixture f;
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "<title>ESP8266 DNS</title>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b>&#9679; ESP8266 DNS</b>"));
    strlcpy(cfg.dev_name, "Sala <1>", sizeof(cfg.dev_name));
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "<title>Sala &lt;1&gt;</title>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b>&#9679; Sala &lt;1&gt;</b>"));

    f.get("/");
    TEST_ASSERT_TRUE(contains(f.out(), "action='/device/save'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='name' value='Sala &lt;1&gt;'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='hostname' value=''"));
}

void test_aparelho_salva_nome_e_hostname() {
    Fixture f;
    f.mqtt.tick(0);                                   // conectando ao broker
    strlcpy(cfg.mqtt_host, "10.0.0.2", sizeof(cfg.mqtt_host));
    f.post("/device/save", {{"name", " Portão "}, {"hostname", " portao-1 "}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Aparelho salvo!"));
    TEST_ASSERT_EQUAL_STRING("Portão", cfg.dev_name);
    TEST_ASSERT_EQUAL_STRING("portao-1", cfg.hostname);
    TEST_ASSERT_EQUAL_STRING("portao-1", WiFi.hostName.c_str());
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"hostname\":\"portao-1\""));

    // Em branco volta ao padrão; o hostname do core fica até reiniciar
    WiFi.hostName = "";
    Fixture g(true, false);
    g.post("/device/save", {{"name", ""}, {"hostname", ""}});
    TEST_ASSERT_EQUAL(200, g.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.dev_name);
    TEST_ASSERT_EQUAL_STRING("", cfg.hostname);
    TEST_ASSERT_EQUAL_STRING("", WiFi.hostName.c_str());
}

void test_aparelho_recusa_nome_ou_hostname_invalido() {
    Fixture f;
    const char* names[] = {"com \"aspas\"", "barra \\", "tab\tx", "del\x7f",
                           "123456789012345678901234567890123"};
    for (const char* n : names) {
        f.post("/device/save", {{"name", n}, {"hostname", "ok"}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "Nome invalido"));
    }
    const char* hosts[] = {"-a", "a-", "com espaco", "a_b", "a.b",
                           "123456789012345678901234567890123"};
    for (const char* h : hosts) {
        f.post("/device/save", {{"name", "ok"}, {"hostname", h}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
        TEST_ASSERT_TRUE(contains(f.out(), "Hostname invalido"));
    }
    TEST_ASSERT_EQUAL_STRING("", cfg.dev_name);
    f.post("/device/save", {{"name", "ok"}, {"hostname", "a"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);

    MockFS::failWriteOpen = true;
    f.post("/device/save", {{"name", "x"}, {"hostname", ""}});
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));
    MockFS::failWriteOpen = false;

    f.srv.request(HTTP_POST, "/device/save", {{"name", "x"}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
    f.srv.authOk = false;
    f.post("/device/save", {{"name", "x"}});
    TEST_ASSERT_EQUAL(401, f.srv.code);
}

void test_cloudflare_save_ok() {
    Fixture f;
    f.post("/cloudflare/save", {
        {"token", "NOVO_TOKEN"}, {"zone", " ZONA "}, {"host", "h.com"}, {"record", " R1 "}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("NOVO_TOKEN", cfg.cf_token);
    TEST_ASSERT_EQUAL_STRING("ZONA", cfg.cf_zone);
    TEST_ASSERT_EQUAL_STRING("h.com", cfg.cf_host);
    TEST_ASSERT_EQUAL_STRING("R1", cfg.cf_record);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"cf_record\":\"R1\""));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "NOVO_TOKEN"));
}

void test_cloudflare_save_token_vazio_mantem_atual() {
    Fixture f;
    strlcpy(cfg.cf_token, "ANTIGO", sizeof(cfg.cf_token));
    f.post("/cloudflare/save", {{"zone", "Z"}, {"record", "R"}});
    TEST_ASSERT_EQUAL_STRING("ANTIGO", cfg.cf_token);
}

void test_cloudflare_save_valor_longo_rejeitado_sem_alterar() {
    Fixture f;
    strlcpy(cfg.cf_zone, "ORIGINAL", sizeof(cfg.cf_zone));
    f.post("/cloudflare/save", {{"zone", "Z"}, {"record", std::string(40, 'r')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "muito longo"));
    TEST_ASSERT_EQUAL_STRING("ORIGINAL", cfg.cf_zone);

    const char* fields[] = {"token", "zone", "host", "record"};
    for (const char* k : fields) {
        f.post("/cloudflare/save", {{k, std::string(64, 'x')}});
        TEST_ASSERT_EQUAL(400, f.srv.code);
    }
}

void test_cloudflare_save_falha_ao_gravar() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/cloudflare/save", {{"zone", "Z"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));
}

// ── /ntfy ────────────────────────────────────────────────────────────────────

void test_ntfy_pagina() {
    Fixture f;
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "Topico nao configurado"));

    strlcpy(cfg.ntfy_topic, "meu_topico", sizeof(cfg.ntfy_topic));
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "Topico: meu_topico"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='meu_topico'"));
}

void test_ntfy_save() {
    Fixture f;
    f.post("/ntfy/save", {{"topic", "a/../b"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Topico invalido"));

    f.post("/ntfy/save", {{"topic", " novo-topico "}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("novo-topico", cfg.ntfy_topic);

    f.post("/ntfy/save", {{"topic", ""}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.ntfy_topic);
}

void test_ntfy_save_falha_ao_gravar() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/ntfy/save", {{"topic", "x"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
}

// ── /mqtt ────────────────────────────────────────────────────────────────────

void test_mqtt_pagina() {
    Fixture f;
    f.get("/mqtt");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "<b>desligado</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "esp8266iot/00dead/state, /cmd, /avail"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='host' value=''"));
    TEST_ASSERT_TRUE(contains(f.out(), "href='/mqtt'"));

    strlcpy(cfg.mqtt_host, "10.0.0.2:1884", sizeof(cfg.mqtt_host));
    strlcpy(cfg.mqtt_user, "casa", sizeof(cfg.mqtt_user));
    strlcpy(cfg.mqtt_pass, "segredo", sizeof(cfg.mqtt_pass));
    f.get("/mqtt");
    TEST_ASSERT_TRUE(contains(f.out(), "value='10.0.0.2:1884'"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='casa'"));
    TEST_ASSERT_FALSE(contains(f.out(), "segredo"));          // a senha nunca volta à página
    TEST_ASSERT_TRUE(contains(f.out(), "<b>conectando</b>"));
}

void test_mqtt_pagina_sem_servico() {
    Fixture f(true, false);
    f.get("/mqtt");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "Situacao"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='host'"));
    f.post("/mqtt/save", {{"host", "10.0.0.2"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

void test_mqtt_save() {
    Fixture f;
    f.post("/mqtt/save", {{"host", " 10.0.0.2:1884 "}, {"user", " casa "}, {"pass", "s3nha"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Configuracao MQTT salva!"));
    TEST_ASSERT_EQUAL_STRING("10.0.0.2:1884", cfg.mqtt_host);
    TEST_ASSERT_EQUAL_STRING("casa", cfg.mqtt_user);
    TEST_ASSERT_EQUAL_STRING("s3nha", cfg.mqtt_pass);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"mqtt_host\":\"10.0.0.2:1884\""));

    // Senha em branco mantém; o interruptor apaga
    f.post("/mqtt/save", {{"host", "10.0.0.2"}, {"user", "casa"}, {"pass", ""}});
    TEST_ASSERT_EQUAL_STRING("s3nha", cfg.mqtt_pass);
    f.post("/mqtt/save", {{"host", "10.0.0.2"}, {"user", "casa"}, {"pass_clear", "1"}});
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt_pass);

    // Em branco desliga
    f.post("/mqtt/save", {{"host", ""}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt_host);
}

void test_mqtt_save_validacoes() {
    Fixture f;
    f.post("/mqtt/save", {{"host", "10.0.0.2:99999"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Broker invalido"));
    f.post("/mqtt/save", {{"host", std::string(64, 'h')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);

    f.post("/mqtt/save", {{"host", "b"}, {"user", std::string(32, 'u')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Usuario ou senha longos demais"));
    f.post("/mqtt/save", {{"host", "b"}, {"pass", std::string(64, 'p')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt_host);

    // No limite ainda vale
    f.post("/mqtt/save", {{"host", std::string(63, 'h')}, {"user", std::string(31, 'u')},
                          {"pass", std::string(63, 'p')}});
    TEST_ASSERT_EQUAL(200, f.srv.code);

    MockFS::failWriteOpen = true;
    f.post("/mqtt/save", {{"host", "b"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));

    // Sem CSRF
    MockFS::failWriteOpen = false;
    f.srv.request(HTTP_POST, "/mqtt/save", {{"host", "x"}});
    TEST_ASSERT_EQUAL(403, f.srv.code);
}

// ── /password ────────────────────────────────────────────────────────────────

void test_password_pagina_primeiro_acesso_sem_autenticacao() {
    cfg.webpss = "";
    cfg.webusr = "";
    Fixture f;
    f.srv.authOk = false; // não deve ser consultado
    f.get("/password");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Defina uma senha"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='admin'"));
}

void test_password_pagina_com_senha() {
    Fixture f;
    f.get("/password");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "Defina uma senha"));
}

void test_password_save_rotaciona_csrf() {
    Fixture f;
    std::string old = f.web.csrfToken();
    f.post("/password/save", {{"user", "novo"}, {"pass1", "segredo123"}, {"pass2", "segredo123"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("novo",       cfg.webusr.c_str());
    TEST_ASSERT_EQUAL_STRING("segredo123", cfg.webpss.c_str());
    TEST_ASSERT_TRUE(old != f.web.csrfToken());
}

void test_password_save_invalido() {
    Fixture f;
    f.post("/password/save", {{"user", "u"}, {"pass1", "segredo123"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    f.post("/password/save", {{"user", std::string(33, 'u')}, {"pass1", "segredo123"}, {"pass2", "segredo123"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    f.post("/password/save", {{"user", "u"}, {"pass1", std::string(65, 'p')}, {"pass2", std::string(65, 'p')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("senha-forte", cfg.webpss.c_str());
}

// ── /update ──────────────────────────────────────────────────────────────────

void test_update_pagina_inicial() {
    Fixture f;
    TEST_ASSERT_TRUE(f.get("/update"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Atualizacao automatica"));
    TEST_ASSERT_TRUE(contains(f.out(), " checked"));
    TEST_ASSERT_TRUE(contains(f.out(), "ligada"));
    TEST_ASSERT_TRUE(contains(f.out(), "Verificar agora"));
    TEST_ASSERT_TRUE(contains(f.out(), "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"));
    TEST_ASSERT_FALSE(contains(f.out(), "http-equiv='refresh'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Instalar esta versao"));
}

void test_update_automatica_desligada() {
    cfg.ota_auto = false;
    Fixture f;
    f.ota.setAutomatic(false);
    f.get("/update");
    TEST_ASSERT_TRUE(contains(f.out(), "desligada"));
    TEST_ASSERT_FALSE(contains(f.out(), " checked"));
}

void test_update_salva_automatica() {
    Fixture f;
    f.post("/update/auto", {});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(cfg.ota_auto);
    TEST_ASSERT_FALSE(f.ota.automatic());
    TEST_ASSERT_TRUE(contains(f.out(), "desligada"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"ota_auto\":false"));

    f.post("/update/auto", {{"ota_auto", "1"}});
    TEST_ASSERT_TRUE(cfg.ota_auto);
    TEST_ASSERT_TRUE(f.ota.automatic());
    TEST_ASSERT_TRUE(contains(f.out(), "ligada"));
}

void test_update_auto_falha_ao_gravar() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/update/auto", {{"ota_auto", "1"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));
}

void test_update_verificar_redireciona_e_mostra_resultado() {
    Fixture f;
    f.post("/update/check");
    TEST_ASSERT_EQUAL(303, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/update", f.srv.headers["Location"].c_str());
    TEST_ASSERT_TRUE(f.ota.pending());

    f.get("/update");
    TEST_ASSERT_TRUE(contains(f.out(), "http-equiv='refresh' content='3'"));
    TEST_ASSERT_TRUE(contains(f.out(), "Consultando o GitHub..."));
    TEST_ASSERT_FALSE(contains(f.out(), "Instalar esta versao"));

    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(0);
    f.get("/update");
    TEST_ASSERT_TRUE(contains(f.out(), "v999"));
    TEST_ASSERT_TRUE(contains(f.out(), "Instalar esta versao"));
    TEST_ASSERT_FALSE(contains(f.out(), "http-equiv='refresh'"));
}

void test_update_instalar_sem_versao() {
    Fixture f;
    f.post("/update/install");
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Nenhuma versao nova"));
}

void test_update_instalar_mostra_progresso() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    MockHTTP::push(200, std::string(1000, 'X'));
    f.post("/update/install");
    TEST_ASSERT_EQUAL(303, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("/update", f.srv.headers["Location"].c_str());

    f.ota.tick(1);
    f.get("/update");
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_TRUE(contains(f.out(), "OTA em progresso"));
    TEST_ASSERT_TRUE(contains(f.out(), "http-equiv='refresh' content='3'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Instalar esta versao"));
}

void test_password_save_falha_ao_gravar() {
    Fixture f;
    std::string old = f.web.csrfToken();
    MockFS::failWriteOpen = true;
    f.post("/password/save", {{"user", "u"}, {"pass1", "segredo123"}, {"pass2", "segredo123"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(old == f.web.csrfToken());
}

// ── /iot ─────────────────────────────────────────────────────────────────────

static void sensorCfg(const char* slug, int pin) {
    strlcpy(cfg.iot.sensor, slug, sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = (int8_t)pin;
}

static void actuatorCfg(const char* slug, int pin) {
    strlcpy(cfg.iot.actuator, slug, sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = (int8_t)pin;
}

void test_iot_pagina_sem_nada_configurado() {
    Fixture f;
    f.get("/iot");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "<h1>IoT</h1>"));
    TEST_ASSERT_TRUE(contains(f.out(), "/pinout.svg"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='n' checked>Nada"));
    TEST_ASSERT_TRUE(contains(f.out(), "<legend>O que este aparelho faz?</legend>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<optgroup label='Temperatura e umidade'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<optgroup label='Cor'><option value='fita'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='-1' selected>Escolha</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "var SK={dht22:3,dht11:2"));
    TEST_ASSERT_TRUE(contains(f.out(), "fita:4},T='"));
    TEST_ASSERT_TRUE(contains(f.out(), f.web.csrfToken()));
    TEST_ASSERT_TRUE(contains(f.out(), "';</script><script src='/iot.js?v=" WEB_IOT_TAG "'></script>"));
    // Sem nada ligado: sem cartão "Agora" (o iot.js só lê ao vivo com ns/na)
    TEST_ASSERT_FALSE(contains(f.out(), "<h2>Agora</h2>"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='ns'"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='na'"));
    TEST_ASSERT_FALSE(contains(f.out(), "http-equiv='refresh'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Cadastre o topico"));
    TEST_ASSERT_FALSE(contains(f.out(), "Tipo desconhecido"));
}

void test_iot_pagina_sensor_e_atuador_com_leitura_ao_vivo() {
    sensorCfg("pir", 2);
    actuatorCfg("rele", 0);
    cfg.iot.notify = true;
    Fixture f;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "Cadastre o topico"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='sa' checked>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='pir' selected>Presenca (PIR)</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='rele' selected>Rele</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<option value='2' selected>GPIO2</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>Agora</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='ns'>aguardando</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='na'>desligado</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='nm'>automatico</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "role='switch' id='sw' aria-checked='false'"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Rele</button>"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='pd' min='1' max='3600' value='1'"));
    TEST_ASSERT_TRUE(contains(f.out(), "onclick=\"c('auto')\" hidden>"));
    TEST_ASSERT_TRUE(contains(f.out(), "role='status' aria-live='polite'"));
    TEST_ASSERT_TRUE(contains(f.out(), "<script src='/iot.js?v=" WEB_IOT_TAG "'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='rule' value='1' checked>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='notify' value='1' checked>"));

    // Ligado à mão: botão marcado e o "voltar ao automático" aparece
    f.iot.command("ligar", 0);
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "aria-checked='true'"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='nm'>manual</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "onclick=\"c('auto')\">Voltar"));

    strlcpy(cfg.ntfy_topic, "topico", sizeof(cfg.ntfy_topic));
    f.get("/iot");
    TEST_ASSERT_FALSE(contains(f.out(), "Cadastre o topico"));
}

void test_iot_pagina_so_sensor_e_so_atuador() {
    sensorCfg("ds18b20", 3);
    Fixture f;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='s' checked>"));
    TEST_ASSERT_TRUE(contains(f.out(), "<b id='ns'>"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='na'"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='sw'"));

    cfg = Config{};
    cfg.webusr = "admin";
    cfg.webpss = "senha-forte";
    actuatorCfg("ventilador", 0);
    cfg.iot.dutyOn = 80;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='a' checked>"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='ns'"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='nm'"));
    TEST_ASSERT_FALSE(contains(f.out(), "c('auto')"));
    TEST_ASSERT_TRUE(contains(f.out(),
        "<label>Intensidade: <output id='lvo'>80</output>%<input type='range' id='lv' "
        "min='0' max='100' value='80'"));
    TEST_ASSERT_TRUE(contains(f.out(), "c('nivel '+this.value)"));
    TEST_ASSERT_FALSE(contains(f.out(), "type='color'"));
}

void test_iot_controles_de_posicao_e_cor() {
    actuatorCfg("servo", 2);
    cfg.iot.angleOff = 15;
    Fixture f;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "onclick=\"c('abrir')\">Abrir</button>"));
    TEST_ASSERT_TRUE(contains(f.out(), "onclick=\"c('fechar')\">Fechar</button>"));
    TEST_ASSERT_FALSE(contains(f.out(), "id='sw'"));
    TEST_ASSERT_TRUE(contains(f.out(), "<output id='ago'>15</output> graus"));
    TEST_ASSERT_TRUE(contains(f.out(), "max='180' value='15'"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='pd' min='1' max='3600' value='3'"));

    actuatorCfg("fita", 1);
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "<label>Brilho: <output id='lvo'>100</output>%"));
    TEST_ASSERT_TRUE(contains(f.out(), "<input type='color' id='co' value='#ffffff'"));
    TEST_ASSERT_TRUE(contains(f.out(), "c('cor '+this.value)"));
}

void test_iot_tipo_desconhecido_avisa() {
    sensorCfg("laser", 2);
    Fixture f;
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "Tipo desconhecido nesta versao"));
    // A configuração continua marcada como sensor: salvar não perde sem querer
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='s' checked>"));

    // Tipo sem pino não conta como em uso
    sensorCfg("pir", -1);
    actuatorCfg("rele", -1);
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='n' checked>"));
}

void test_iot_now_em_json() {
    sensorCfg("pir", 2);
    actuatorCfg("rele", 0);
    Fixture f;
    mock::pinIn[2] = HIGH;
    f.iot.begin();
    f.iot.tick(0);
    f.iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(f.get("/iot/now"));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("application/json", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("no-store", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_TRUE(contains(f.out(), "\"sensor\":{\"type\":\"pir\",\"status\":\"ok\""));
    TEST_ASSERT_TRUE(contains(f.out(), "\"on\":true,\"mode\":\"auto\""));

    f.srv.authOk = false;   // leitura do aparelho exige login
    f.get("/iot/now");
    TEST_ASSERT_EQUAL(401, f.srv.code);
}

void test_iot_cmd_do_painel() {
    actuatorCfg("rele", 0);
    Fixture f;
    f.iot.begin();
    TEST_ASSERT_TRUE(f.post("/iot/cmd", {{"cmd", "ligar"}}));
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "\"on\":true"));
    TEST_ASSERT_TRUE(f.iot.actuatorOn());

    f.post("/iot/cmd", {{"cmd", "voar"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"comando invalido\"}", f.out().c_str());
    f.post("/iot/cmd", {{"cmd", "angulo 30"}});
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"comando nao vale para este atuador\"}", f.out().c_str());

    actuatorCfg("", -1);
    f.iot.begin();
    f.post("/iot/cmd", {{"cmd", "ligar"}});
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"nenhum atuador configurado\"}", f.out().c_str());
}

// Formulário completo de sensor e atuador; cada teste muda o que precisa
static Args iotForm() {
    return {
        {"role", "sa"}, {"sensor", "pir"}, {"sensor_pin", "2"}, {"sensor_high", "1"},
        {"sensor_on", ""}, {"sensor_off", ""}, {"metric", "0"},
        {"actuator", "rele"}, {"actuator_pin", "0"}, {"actuator_high", "0"},
        {"duty_on", "100"}, {"duty_off", "0"}, {"angle_on", "90"}, {"angle_off", "0"},
        {"leds", "8"}, {"max_on", "0"}, {"restore", "0"}, {"rule", "1"},
        {"toggle", "0"}, {"above", "0"}, {"threshold", "0"}, {"margin", "0"},
        {"linear", "0"}, {"threshold_hi", "0"},
    };
}

static void saveExpectError(Fixture& f, Args a, const char* msg) {
    IoTConfig before = cfg.iot;
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL_MESSAGE(400, f.srv.code, msg);
    TEST_ASSERT_TRUE_MESSAGE(contains(f.out(), msg), msg);
    TEST_ASSERT_EQUAL_STRING(before.sensor, cfg.iot.sensor);
    TEST_ASSERT_EQUAL_STRING(before.actuator, cfg.iot.actuator);
}

void test_iot_save_sensor_e_atuador_digital() {
    Fixture f;
    Args a = iotForm();
    a["sensor_on"] = " molhado ";
    a["sensor_off"] = "seco-1.0,x_y";
    a["sensor_high"] = "0";
    a["actuator_high"] = "1";
    a["toggle"] = "1";
    a["max_on"] = "30";
    a["restore"] = "1";
    a["notify"] = "1";
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT configurado"));
    const IoTConfig& t = cfg.iot;
    TEST_ASSERT_EQUAL_STRING("pir", t.sensor);
    TEST_ASSERT_EQUAL(2, t.sensorPin);
    TEST_ASSERT_FALSE(t.sensorHigh);
    TEST_ASSERT_EQUAL_STRING("molhado", t.onText);
    TEST_ASSERT_EQUAL_STRING("seco-1.0,x_y", t.offText);
    TEST_ASSERT_EQUAL_STRING("rele", t.actuator);
    TEST_ASSERT_EQUAL(0, t.actuatorPin);
    TEST_ASSERT_TRUE(t.actuatorHigh);
    TEST_ASSERT_EQUAL(30, t.maxOn);
    TEST_ASSERT_TRUE(t.restore);
    TEST_ASSERT_TRUE(t.rule);
    TEST_ASSERT_TRUE(t.toggle);
    TEST_ASSERT_TRUE(t.notify);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"sensor\":\"pir\""));
    TEST_ASSERT_TRUE(f.iot.hasRule());                 // já aplicado

    // Sem regra: o modo do sensor digital não é lido
    a["rule"] = "0";
    a["toggle"] = "0";
    f.post("/iot/save", a);
    TEST_ASSERT_FALSE(cfg.iot.rule);
    TEST_ASSERT_TRUE(cfg.iot.toggle);
}

void test_iot_save_numerico_com_intensidade_proporcional() {
    Fixture f;
    Args a = iotForm();
    a["sensor"] = "dht22";
    a["metric"] = "1";
    a["actuator"] = "ventilador";
    a["duty_on"] = "90";
    a["duty_off"] = "10";
    a["above"] = "1";
    a["threshold"] = "-5";
    a["margin"] = "3";
    a["linear"] = "1";
    a["threshold_hi"] = "30";
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT configurado"));
    const IoTConfig& t = cfg.iot;
    TEST_ASSERT_EQUAL(1, t.metric);
    TEST_ASSERT_EQUAL(90, t.dutyOn);
    TEST_ASSERT_EQUAL(10, t.dutyOff);
    TEST_ASSERT_TRUE(t.above);
    TEST_ASSERT_EQUAL(-5, t.threshold);
    TEST_ASSERT_EQUAL(3, t.margin);
    TEST_ASSERT_TRUE(t.linear);
    TEST_ASSERT_EQUAL(30, t.thresholdHi);

    // Proporcional só existe na intensidade, e só se pedido
    a["actuator"] = "rele";
    f.post("/iot/save", a);
    TEST_ASSERT_FALSE(cfg.iot.linear);
    a["actuator"] = "motor";
    a["linear"] = "0";
    f.post("/iot/save", a);
    TEST_ASSERT_FALSE(cfg.iot.linear);
    a["linear"] = "1";

    // DHT11 também lê a grandeza; outros sensores não
    a["sensor"] = "dht11";
    a["metric"] = "0";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(0, cfg.iot.metric);
    a["sensor"] = "distancia";
    a["metric"] = "9";
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT configurado"));
}

void test_iot_save_posicao_cor_e_papeis() {
    Fixture f;
    Args a = iotForm();
    a["role"] = "a";
    a["actuator"] = "servo";
    a["angle_on"] = "170";
    a["angle_off"] = "5";
    a["sensor"] = "lixo";                              // ignorado: só atuador
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT configurado"));
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.sensor);
    TEST_ASSERT_EQUAL(-1, cfg.iot.sensorPin);
    TEST_ASSERT_EQUAL(170, cfg.iot.angleOn);
    TEST_ASSERT_EQUAL(5, cfg.iot.angleOff);

    a["actuator"] = "fita";
    a["leds"] = "60";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL(60, cfg.iot.leds);

    a["role"] = "s";
    a["sensor"] = "contato";
    a["actuator_pin"] = "2";                           // o pino do atuador não conta
    f.post("/iot/save", a);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT configurado"));
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.actuator);
    TEST_ASSERT_EQUAL(-1, cfg.iot.actuatorPin);

    a["role"] = "n";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.sensor);
    TEST_ASSERT_EQUAL(60, cfg.iot.leds);               // ajustes ficam guardados
}

void test_iot_save_rejeita_valores() {
    Fixture f;
    struct { const char* key; const char* value; const char* msg; } cases[] = {
        {"role", "x", "Escolha o que o aparelho faz"},
        {"sensor", "laser", "Tipo de sensor invalido"},
        {"sensor_pin", "-1", "Escolha o pino"},
        {"sensor_pin", "7", "Escolha o pino"},
        {"sensor_on", "0123456789abcdef", "Nome de estado invalido"},
        {"sensor_off", "<b>", "Nome de estado invalido"},
        {"actuator", "teleporte", "Tipo de atuador invalido"},
        {"actuator_pin", "-1", "Escolha o pino"},
        {"actuator_pin", "2", "pinos diferentes"},
        {"max_on", "1441", "Valor invalido"},
        {"threshold", "10000", "Valor invalido"},
        {"margin", "51", "Valor invalido"},
    };
    for (const auto& c : cases) {
        Args a = iotForm();
        a["sensor"] = "ds18b20";
        a[c.key] = c.value;
        if (strcmp(c.key, "sensor_on") == 0 || strcmp(c.key, "sensor_off") == 0)
            a["sensor"] = "pir";
        saveExpectError(f, a, c.msg);
    }

    Args dht = iotForm();
    dht["sensor"] = "dht22";
    dht["metric"] = "2";
    saveExpectError(f, dht, "Valor invalido");

    const char* levelKeys[] = {"duty_on", "duty_off"};
    for (const char* k : levelKeys) {
        Args a = iotForm();
        a["actuator"] = "motor";
        a[k] = "101";
        saveExpectError(f, a, "Valor invalido");
    }
    const char* angleKeys[] = {"angle_on", "angle_off"};
    for (const char* k : angleKeys) {
        Args a = iotForm();
        a["actuator"] = "servo";
        a[k] = "181";
        saveExpectError(f, a, "Valor invalido");
    }
    Args leds = iotForm();
    leds["actuator"] = "fita";
    leds["leds"] = "0";
    saveExpectError(f, leds, "Valor invalido");

    Args hi = iotForm();
    hi["sensor"] = "distancia";
    hi["actuator"] = "led";
    hi["linear"] = "1";
    hi["threshold"] = "50";
    hi["threshold_hi"] = "abc";
    saveExpectError(f, hi, "Valor invalido");
    hi["threshold_hi"] = "50";
    saveExpectError(f, hi, "O limite alto precisa passar do limite");
}

void test_iot_save_falha_ao_gravar() {
    Fixture f;
    MockFS::failWriteOpen = true;
    f.post("/iot/save", iotForm());
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));
    TEST_ASSERT_FALSE(f.iot.hasActuator());            // a saída no ar não muda
}

// ── API local ────────────────────────────────────────────────────────────────

static const char* TOKEN = "0123456789abcdef0123456789abcdef";

static bool api(Fixture& f, int method, const char* path, const char* auth, Args a = {}) {
    std::map<std::string, std::string> h;
    if (auth) h["Authorization"] = auth;
    return f.srv.request(method, path, a, h);
}

void test_api_so_existe_com_iot_configurado() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    Fixture f;
    TEST_ASSERT_TRUE(api(f, HTTP_GET, "/api/state", "Bearer 0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_EQUAL(404, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"nenhum sensor ou atuador configurado\"}", f.out().c_str());
    api(f, HTTP_POST, "/api/cmd", "Bearer 0123456789abcdef0123456789abcdef", {{"cmd", "ligar"}});
    TEST_ASSERT_EQUAL(404, f.srv.code);
}

void test_api_exige_o_token() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    sensorCfg("pir", 2);
    Fixture f;
    f.iot.begin();
    const char* bad[] = {
        nullptr,
        "Bearer 0123456789abcdef0123456789abcdee",     // último caractere
        "Bearer 0123456789abcdef",                     // curto
        "Basic 0123456789abcdef0123456789abcdef",      // outro esquema
        "bearer 0123456789abcdef0123456789abcdef",
    };
    for (const char* auth : bad) {
        api(f, HTTP_GET, "/api/state", auth);
        TEST_ASSERT_EQUAL(401, f.srv.code);
        TEST_ASSERT_EQUAL_STRING("Bearer", f.srv.headers["WWW-Authenticate"].c_str());
        TEST_ASSERT_EQUAL_STRING("{\"error\":\"token invalido\"}", f.out().c_str());
        api(f, HTTP_GET, "/api/state", (std::string("Bearer ") + TOKEN).c_str());              // zera a conta de erros
    }
    // O login do painel não vale na API
    TEST_ASSERT_TRUE(f.srv.authOk);
    api(f, HTTP_GET, "/api/state", nullptr);
    TEST_ASSERT_EQUAL(401, f.srv.code);

    // Sem token gerado ninguém entra, nem com "Bearer " vazio
    cfg.api_token[0] = '\0';
    api(f, HTTP_GET, "/api/state", "Bearer ");
    TEST_ASSERT_EQUAL(401, f.srv.code);
}

void test_api_le_e_comanda() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("rele", 0);
    Fixture f;
    f.iot.begin();
    const char* auth = "Bearer 0123456789abcdef0123456789abcdef";

    api(f, HTTP_GET, "/api/state", auth);
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("application/json", f.srv.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("no-store", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_TRUE(contains(f.out(), "\"api\":1,\"sensor\":null"));
    TEST_ASSERT_TRUE(contains(f.out(), "\"type\":\"rele\""));

    api(f, HTTP_POST, "/api/cmd", auth, {{"cmd", "ligar"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(f.iot.actuatorOn());

    // curl -d desligar: o core põe o corpo em "plain"
    api(f, HTTP_POST, "/api/cmd", auth, {{"plain", "desligar"}});
    TEST_ASSERT_FALSE(f.iot.actuatorOn());

    api(f, HTTP_POST, "/api/cmd", auth, {{"plain", "50%"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"comando nao vale para este atuador\"}", f.out().c_str());
    api(f, HTTP_POST, "/api/cmd", auth);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"comando invalido\"}", f.out().c_str());
}

void test_api_token_gerado_mostrado_e_trocado() {
    Fixture f;
    f.post("/iot/save", iotForm());
    TEST_ASSERT_EQUAL(32, (int)std::strlen(cfg.api_token));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], cfg.api_token));
    std::string first = cfg.api_token;

    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>API local</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), ("readonly value='" + first + "'").c_str()));
    TEST_ASSERT_TRUE(contains(f.out(),
        ("curl -H 'Authorization: Bearer " + first + "' http://192.168.1.1/api/state").c_str()));
    TEST_ASSERT_TRUE(contains(f.out(), "' -d ligar http://192.168.1.1/api/cmd"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Gerar token novo</button>"));

    // Salvar de novo mantém o token
    f.post("/iot/save", iotForm());
    TEST_ASSERT_EQUAL_STRING(first.c_str(), cfg.api_token);

    f.post("/iot/token");
    TEST_ASSERT_TRUE(contains(f.out(), "Token novo gerado"));
    TEST_ASSERT_EQUAL(32, (int)std::strlen(cfg.api_token));
    TEST_ASSERT_FALSE(first == cfg.api_token);

    MockFS::failWriteOpen = true;
    f.post("/iot/token");
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));
}

void test_api_sem_token_e_iot_desligado() {
    // IoT desligado não gera token
    Fixture f;
    Args a = iotForm();
    a["role"] = "n";
    f.post("/iot/save", a);
    TEST_ASSERT_EQUAL_STRING("", cfg.api_token);
    f.get("/iot");
    TEST_ASSERT_FALSE(contains(f.out(), "API local"));

    // Configurado por migração (sem token): oferece gerar
    actuatorCfg("rele", 0);
    f.iot.begin();
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>API local</h2>"));
    TEST_ASSERT_FALSE(contains(f.out(), "curl"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Gerar token</button>"));
}

void test_ntfy_comandos_pagina_e_validacoes() {
    Fixture f;
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "<h2>Comandos pelo ntfy</h2>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Situacao</span><b>desligado</b>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='cmd' value=''"));
    TEST_ASSERT_TRUE(contains(f.out(), "Gerar nome aleatorio"));
    TEST_ASSERT_TRUE(contains(f.out(), "crypto.getRandomValues"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='token'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='token_clear' value='1'>"));

    struct { const char* topic; const char* cmd; const char* token; const char* msg; } bad[] = {
        {"dados", "a b", "", "Topico de comandos invalido"},
        {"mesmo-topico-bem-longo", "mesmo-topico-bem-longo", "", "precisa ser diferente"},
        {"dados", "cmd-curto-de-19-ch", "", "Topico de comandos curto"},
        {"dados", "cmd-x", "tk-1", "Token do ntfy invalido"},
        {"dados", "cmd-x", "tk_0123456789012345678901234567890123456789012345", "Token do ntfy invalido"},
    };
    for (const auto& b : bad) {
        f.post("/ntfy/save", {{"topic", b.topic}, {"cmd", b.cmd}, {"token", b.token}});
        TEST_ASSERT_EQUAL_MESSAGE(400, f.srv.code, b.msg);
        TEST_ASSERT_TRUE_MESSAGE(contains(f.out(), b.msg), b.msg);
    }
    TEST_ASSERT_EQUAL_STRING("", cfg.ntfy_cmd);

    f.post("/ntfy/save", {{"topic", "dados"}, {"cmd", " cmd-x "}, {"token", "tk_abc"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("cmd-x", cfg.ntfy_cmd);
    TEST_ASSERT_EQUAL_STRING("tk_abc", cfg.ntfy_token);
    f.get("/ntfy");
    TEST_ASSERT_FALSE(contains(f.out(), "tk_abc"));        // segredo não volta à página

    // Em branco mantém; marcar apaga
    f.post("/ntfy/save", {{"topic", "dados"}, {"cmd", ""}, {"token", ""}});
    TEST_ASSERT_EQUAL_STRING("tk_abc", cfg.ntfy_token);
    TEST_ASSERT_EQUAL_STRING("", cfg.ntfy_cmd);
    f.post("/ntfy/save", {{"topic", "dados"}, {"token", ""}, {"token_clear", "1"}});
    TEST_ASSERT_EQUAL_STRING("", cfg.ntfy_token);

    // Com atuador e tópico, mostra a conexão
    strlcpy(cfg.iot.actuator, "rele", sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));
    f.iot.begin();
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "Situacao</span><b>conectando</b>"));
}

void test_ntfy_pagina_sem_servico_de_comandos() {
    Fixture f(false);
    f.get("/ntfy");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(contains(f.out(), "Comandos pelo ntfy"));
}

void test_iot_descadastrar_pede_confirmacao() {
    Fixture f;
    Args a = iotForm();
    a["restore"] = "1";
    f.post("/iot/save", a);
    f.post("/iot/cmd", {{"cmd", "ligar"}});
    TEST_ASSERT_TRUE(f.iot.actuatorOn());
    TEST_ASSERT_TRUE(MockFS::files.count("/iot.state") == 1);
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(),
        "<form action='/iot/clear' method='POST'>"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Descadastrar sensor e atuador</button>"));

    // Primeiro passo só mostra o aviso; nada muda
    f.post("/iot/clear");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "<h1>Descadastrar</h1>"));
    TEST_ASSERT_TRUE(contains(f.out(), "serao apagados"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='confirm' value='1'"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Sim, descadastrar</button>"));
    TEST_ASSERT_TRUE(contains(f.out(), "href='/iot'>Voltar</a>"));
    TEST_ASSERT_EQUAL_STRING("pir", cfg.iot.sensor);
    TEST_ASSERT_TRUE(f.iot.actuatorOn());

    // Confirmado: apaga tudo, desliga o atuador e a API some
    std::string token = cfg.api_token;
    f.post("/iot/clear", {{"confirm", "1"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Sensor e atuador descadastrados"));
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.sensor);
    TEST_ASSERT_EQUAL_STRING("", cfg.iot.actuator);
    TEST_ASSERT_EQUAL(-1, cfg.iot.sensorPin);
    TEST_ASSERT_EQUAL(-1, cfg.iot.actuatorPin);
    TEST_ASSERT_EQUAL_STRING("", cfg.api_token);
    TEST_ASSERT_FALSE(contains(MockFS::files["/config.json"], token.c_str()));
    TEST_ASSERT_TRUE(MockFS::files.count("/iot.state") == 0);
    TEST_ASSERT_FALSE(f.iot.hasSensor());
    TEST_ASSERT_FALSE(f.iot.hasActuator());
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);  // relé ativo em baixo: desligado

    f.get("/iot");
    TEST_ASSERT_FALSE(contains(f.out(), "/iot/clear"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='role' value='n' checked>Nada"));
}

void test_iot_descadastrar_falha_ao_gravar() {
    Fixture f;
    f.post("/iot/save", iotForm());
    MockFS::failWriteOpen = true;
    f.post("/iot/clear", {{"confirm", "1"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));
    TEST_ASSERT_TRUE(f.iot.hasActuator());  // o que está no ar não muda
}

void test_status_mostra_a_senha_do_ap() {
    Fixture f;
    MockFS::put(WiFiService::AP_KEY_PATH, "abcd2345wxyz");
    f.get("/status");
    TEST_ASSERT_TRUE(contains(f.out(), "Senha do AP ESP_Config"));
    TEST_ASSERT_TRUE(contains(f.out(), "abcd2345wxyz"));
    f.srv.authOk = false;                                     // só com login
    f.get("/status");
    TEST_ASSERT_EQUAL(401, f.srv.code);
}

// ── Endurecimento para acesso remoto ─────────────────────────────────────────

static const char* OK_BASIC  = "Basic YWRtaW46c2VuaGEtZm9ydGU=";   // admin:senha-forte
static const char* BAD_BASIC = "Basic YWRtaW46ZXJyYWRh";           // admin:errada

static int statusWith(Fixture& f, const char* auth) {
    f.srv.request(HTTP_GET, "/status", {}, {{"Authorization", auth}});
    return f.srv.code;
}

void test_login_erros_seguidos_esperam_cada_vez_mais() {
    Fixture f;
    f.srv.authOk = false;
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) TEST_ASSERT_EQUAL(401, statusWith(f, BAD_BASIC));
    TEST_ASSERT_TRUE(logHas(logger, "Login: erros seguidos"));

    // Na espera, nem a senha certa é testada
    TEST_ASSERT_EQUAL(429, statusWith(f, OK_BASIC));
    TEST_ASSERT_TRUE(contains(f.out(), "Muitas tentativas"));
    setMillis(WebService::LOGIN_WAIT_MS - 1);
    TEST_ASSERT_EQUAL(429, statusWith(f, BAD_BASIC));
    setMillis(WebService::LOGIN_WAIT_MS);
    TEST_ASSERT_EQUAL(401, statusWith(f, BAD_BASIC));          // 6º erro: espera dobra
    unsigned long t = WebService::LOGIN_WAIT_MS;
    setMillis(t + 2 * WebService::LOGIN_WAIT_MS - 1);
    TEST_ASSERT_EQUAL(429, statusWith(f, OK_BASIC));
    setMillis(t + 2 * WebService::LOGIN_WAIT_MS);
    TEST_ASSERT_EQUAL(200, statusWith(f, OK_BASIC));           // acertar zera a conta

    for (int i = 0; i < WebService::FREE_LOGIN_FAILS - 1; i++) TEST_ASSERT_EQUAL(401, statusWith(f, BAD_BASIC));
    TEST_ASSERT_EQUAL(200, statusWith(f, OK_BASIC));
}

void test_login_espera_tem_teto_e_pedido_sem_credencial_nao_conta() {
    Fixture f;
    f.srv.authOk = false;
    // O primeiro pedido do navegador vem sem credencial: não conta
    for (int i = 0; i < 20; i++) TEST_ASSERT_EQUAL(401, statusWith(f, ""));
    TEST_ASSERT_FALSE(logHas(logger, "Login: erros seguidos"));

    unsigned long t = 0;
    for (int i = 0; i < 260; i++) {                           // até o contador parar em 255
        statusWith(f, BAD_BASIC);
        t += WebService::LOGIN_WAIT_MAX_MS;                       // sempre depois da espera
        setMillis(t);
    }
    statusWith(f, BAD_BASIC);
    setMillis(t + WebService::LOGIN_WAIT_MAX_MS - 1);
    TEST_ASSERT_EQUAL(429, statusWith(f, OK_BASIC));
    setMillis(t + WebService::LOGIN_WAIT_MAX_MS);
    TEST_ASSERT_EQUAL(200, statusWith(f, OK_BASIC));           // nunca mais que 5 min
}

void test_token_errado_da_api_conta_como_erro() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("rele", 0);
    Fixture f;
    f.iot.begin();
    for (int i = 0; i < WebService::FREE_LOGIN_FAILS; i++) {
        api(f, HTTP_GET, "/api/state", "Bearer ffffffffffffffffffffffffffffffff");
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    api(f, HTTP_GET, "/api/state", (std::string("Bearer ") + TOKEN).c_str());
    TEST_ASSERT_EQUAL(429, f.srv.code);
    setMillis(WebService::LOGIN_WAIT_MS);
    api(f, HTTP_GET, "/api/state", (std::string("Bearer ") + TOKEN).c_str());
    TEST_ASSERT_EQUAL(200, f.srv.code);
}

void test_ntfy_topico_de_comandos_curto_sem_token() {
    Fixture f;
    std::string shortTopic(WebService::MIN_CMD_TOPIC - 1, 'c');
    std::string longTopic(WebService::MIN_CMD_TOPIC, 'c');
    f.post("/ntfy/save", {{"topic", "dados"}, {"cmd", shortTopic}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Topico de comandos curto"));

    f.post("/ntfy/save", {{"topic", "dados"}, {"cmd", longTopic}});
    TEST_ASSERT_EQUAL(200, f.srv.code);

    // Com token (novo ou já salvo), o curto vale
    f.post("/ntfy/save", {{"topic", "dados"}, {"cmd", shortTopic}, {"token", "tk_abc"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    f.post("/ntfy/save", {{"topic", "dados"}, {"cmd", shortTopic}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    // Apagar o token e manter o curto, não
    f.post("/ntfy/save", {{"topic", "dados"}, {"cmd", shortTopic}, {"token_clear", "1"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("tk_abc", cfg.ntfy_token);
}

void test_ntfy_avisa_topico_curto_ja_salvo() {
    Fixture f;
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));       // de antes da regra
    f.get("/ntfy");
    TEST_ASSERT_TRUE(contains(f.out(), "Topico de comandos curto e sem token"));
    strlcpy(cfg.ntfy_token, "tk_abc", sizeof(cfg.ntfy_token));
    f.get("/ntfy");
    TEST_ASSERT_FALSE(contains(f.out(), "curto e sem token"));
    cfg.ntfy_token[0] = '\0';
    strlcpy(cfg.ntfy_cmd, std::string(WebService::MIN_CMD_TOPIC, 'c').c_str(), sizeof(cfg.ntfy_cmd));
    f.get("/ntfy");
    TEST_ASSERT_FALSE(contains(f.out(), "curto e sem token"));
}

void test_cabecalhos_de_seguranca() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("rele", 0);
    Fixture f;
    f.iot.begin();
    f.get("/status");
    TEST_ASSERT_EQUAL_STRING("no-store", f.srv.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL_STRING("DENY", f.srv.headers["X-Frame-Options"].c_str());
    TEST_ASSERT_EQUAL_STRING("nosniff", f.srv.headers["X-Content-Type-Options"].c_str());
    api(f, HTTP_GET, "/api/state", (std::string("Bearer ") + TOKEN).c_str());
    TEST_ASSERT_EQUAL_STRING("DENY", f.srv.headers["X-Frame-Options"].c_str());
    TEST_ASSERT_EQUAL_STRING("no-store", f.srv.headers["Cache-Control"].c_str());
}

void test_log_diz_de_onde_veio_o_comando() {
    strlcpy(cfg.api_token, TOKEN, sizeof(cfg.api_token));
    actuatorCfg("rele", 0);
    Fixture f;
    f.iot.begin();
    f.post("/iot/cmd", {{"cmd", "ligar"}});
    TEST_ASSERT_TRUE(logHas(logger, "Painel: comando \"ligar\""));
    api(f, HTTP_POST, "/api/cmd", (std::string("Bearer ") + TOKEN).c_str(), {{"cmd", "desligar"}});
    TEST_ASSERT_TRUE(logHas(logger, "API: comando \"desligar\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_escape);
    RUN_TEST(test_mask_secret);
    RUN_TEST(test_format_duration);
    RUN_TEST(test_format_bytes);
    RUN_TEST(test_begin_abre_a_porta_e_gera_csrf_aleatorio);
    RUN_TEST(test_tick_atende_clientes);
    RUN_TEST(test_rota_desconhecida);
    RUN_TEST(test_sem_senha_redireciona_para_definir_senha);
    RUN_TEST(test_autenticacao_obrigatoria);
    RUN_TEST(test_autenticacao_basic);
    RUN_TEST(test_csrf_invalido_rejeitado_em_todos_os_posts);
    RUN_TEST(test_root_modo_estacao);
    RUN_TEST(test_root_com_ip_e_ultima_atualizacao);
    RUN_TEST(test_root_mostra_progresso_ota);
    RUN_TEST(test_root_modo_configuracao_primeiro_acesso);
    RUN_TEST(test_root_modo_configuracao_preserva_usuario);
    RUN_TEST(test_root_modo_configuracao_com_senha_definida);
    RUN_TEST(test_save_wifi_ok);
    RUN_TEST(test_save_wifi_rede_aberta);
    RUN_TEST(test_save_wifi_validacoes);
    RUN_TEST(test_save_wifi_primeiro_acesso_exige_senha_do_painel);
    RUN_TEST(test_save_wifi_falha_ao_gravar);
    RUN_TEST(test_imagem_do_aparelho_embutida);
    RUN_TEST(test_css_e_js_em_gzip);
    RUN_TEST(test_pinos_embutidos_em_svg_gzip);
    RUN_TEST(test_log_escapa_conteudo);
    RUN_TEST(test_status_conectado);
    RUN_TEST(test_status_desconectado_e_ota);
    RUN_TEST(test_status_ip_publico);
    RUN_TEST(test_cloudflare_sem_token);
    RUN_TEST(test_cloudflare_com_token_mascarado_e_valores_escapados);
    RUN_TEST(test_nome_no_titulo_e_no_topo);
    RUN_TEST(test_aparelho_salva_nome_e_hostname);
    RUN_TEST(test_aparelho_recusa_nome_ou_hostname_invalido);
    RUN_TEST(test_cloudflare_save_ok);
    RUN_TEST(test_cloudflare_save_token_vazio_mantem_atual);
    RUN_TEST(test_cloudflare_save_valor_longo_rejeitado_sem_alterar);
    RUN_TEST(test_cloudflare_save_falha_ao_gravar);
    RUN_TEST(test_ntfy_pagina);
    RUN_TEST(test_ntfy_save);
    RUN_TEST(test_ntfy_save_falha_ao_gravar);
    RUN_TEST(test_password_pagina_primeiro_acesso_sem_autenticacao);
    RUN_TEST(test_password_pagina_com_senha);
    RUN_TEST(test_password_save_rotaciona_csrf);
    RUN_TEST(test_password_save_invalido);
    RUN_TEST(test_password_save_falha_ao_gravar);
    RUN_TEST(test_update_pagina_inicial);
    RUN_TEST(test_update_automatica_desligada);
    RUN_TEST(test_update_salva_automatica);
    RUN_TEST(test_update_auto_falha_ao_gravar);
    RUN_TEST(test_update_verificar_redireciona_e_mostra_resultado);
    RUN_TEST(test_update_instalar_sem_versao);
    RUN_TEST(test_update_instalar_mostra_progresso);
    RUN_TEST(test_iot_pagina_sem_nada_configurado);
    RUN_TEST(test_iot_pagina_sensor_e_atuador_com_leitura_ao_vivo);
    RUN_TEST(test_iot_pagina_so_sensor_e_so_atuador);
    RUN_TEST(test_iot_controles_de_posicao_e_cor);
    RUN_TEST(test_iot_tipo_desconhecido_avisa);
    RUN_TEST(test_iot_now_em_json);
    RUN_TEST(test_iot_cmd_do_painel);
    RUN_TEST(test_iot_save_sensor_e_atuador_digital);
    RUN_TEST(test_iot_save_numerico_com_intensidade_proporcional);
    RUN_TEST(test_iot_save_posicao_cor_e_papeis);
    RUN_TEST(test_iot_save_rejeita_valores);
    RUN_TEST(test_iot_save_falha_ao_gravar);
    RUN_TEST(test_api_so_existe_com_iot_configurado);
    RUN_TEST(test_api_exige_o_token);
    RUN_TEST(test_api_le_e_comanda);
    RUN_TEST(test_api_token_gerado_mostrado_e_trocado);
    RUN_TEST(test_api_sem_token_e_iot_desligado);
    RUN_TEST(test_ntfy_comandos_pagina_e_validacoes);
    RUN_TEST(test_ntfy_pagina_sem_servico_de_comandos);
    RUN_TEST(test_iot_descadastrar_pede_confirmacao);
    RUN_TEST(test_iot_descadastrar_falha_ao_gravar);
    RUN_TEST(test_mqtt_pagina);
    RUN_TEST(test_mqtt_pagina_sem_servico);
    RUN_TEST(test_mqtt_save);
    RUN_TEST(test_mqtt_save_validacoes);
    RUN_TEST(test_status_mostra_a_senha_do_ap);
    RUN_TEST(test_login_erros_seguidos_esperam_cada_vez_mais);
    RUN_TEST(test_login_espera_tem_teto_e_pedido_sem_credencial_nao_conta);
    RUN_TEST(test_token_errado_da_api_conta_como_erro);
    RUN_TEST(test_ntfy_topico_de_comandos_curto_sem_token);
    RUN_TEST(test_ntfy_avisa_topico_curto_ja_salvo);
    RUN_TEST(test_cabecalhos_de_seguranca);
    RUN_TEST(test_log_diz_de_onde_veio_o_comando);
    return UNITY_END();
}
