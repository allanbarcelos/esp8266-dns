#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "OTAService.h"
#include "WebService.h"

using Args = std::map<std::string, std::string>;

static Logger logger;
static Config cfg;

struct Fixture {
    ConfigStore store{cfg, logger};
    WiFiService wifi{cfg, logger};
    OTAService  ota{logger, wifi};
    IoTService  iot{cfg, logger};
    WebService  web{cfg, logger, store, wifi, ota, iot};
    ESP8266WebServer& srv = *ESP8266WebServer::instance();

    Fixture() { web.begin(); }

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

void test_begin_registra_rotas_e_gera_csrf_aleatorio() {
    Fixture f;
    TEST_ASSERT_TRUE(f.srv.started);
    TEST_ASSERT_EQUAL(16, (int)f.srv.routes.size());
    TEST_ASSERT_EQUAL(16, (int)std::strlen(f.web.csrfToken()));

    std::string first = f.web.csrfToken();
    Fixture g;
    TEST_ASSERT_TRUE(first != g.web.csrfToken());
}

void test_tick_atende_clientes() {
    Fixture f;
    f.web.tick();
    TEST_ASSERT_EQUAL(1, f.srv.handleCalls);
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
    const char* paths[] = {"/", "/status", "/log", "/cloudflare", "/ntfy", "/password", "/update", "/iot"};
    for (const char* p : paths) {
        f.get(p);
        TEST_ASSERT_TRUE(f.srv.authRequested);
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install", "/iot/save"};
    for (const char* p : posts) {
        f.post(p);
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    TEST_ASSERT_EQUAL_STRING("admin",       f.srv.lastAuthUser.c_str());
    TEST_ASSERT_EQUAL_STRING("senha-forte", f.srv.lastAuthPass.c_str());
}

void test_csrf_invalido_rejeitado_em_todos_os_posts() {
    Fixture f;
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install", "/iot/save"};
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
    TEST_ASSERT_TRUE(contains(f.out(), "docs/esp-01.jpg"));
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
    TEST_ASSERT_TRUE(contains(f.out(), "docs/esp-01.jpg"));
    TEST_ASSERT_TRUE(contains(f.out(), "Firmware para o modulo ESP-01"));
    TEST_ASSERT_TRUE(contains(f.out(), "Modo de configuracao"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='pass1'"));      // define senha do painel
    TEST_ASSERT_TRUE(contains(f.out(), "value='admin'"));
    TEST_ASSERT_TRUE(contains(f.out(), f.web.csrfToken()));
    // SSID vazio não pode virar pedaço vazio (terminador do chunked)
    TEST_ASSERT_TRUE(contains(f.out(), "name='ssid' value=''"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='pass'"));
    TEST_ASSERT_TRUE(contains(f.out(), "</html>"));
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
    TEST_ASSERT_TRUE(contains(f.out(), "Heap livre</span><span class='kv-value text-end ms-3'>48.8 KB"));
    TEST_ASSERT_TRUE(contains(f.out(), "Maior bloco</span><span class='kv-value text-end ms-3'>39.1 KB"));
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
    TEST_ASSERT_TRUE(contains(f.out(), "kv-value"));
    TEST_ASSERT_TRUE(contains(f.out(), "color:#ffffff"));
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

// ── /cloudflare ──────────────────────────────────────────────────────────────

void test_cloudflare_sem_token() {
    Fixture f;
    f.get("/cloudflare");
    TEST_ASSERT_TRUE(contains(f.out(), "Token nao configurado"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='record4'"));
}

void test_cloudflare_com_token_mascarado_e_valores_escapados() {
    Fixture f;
    strlcpy(cfg.cf_token, "segredo-abcd", sizeof(cfg.cf_token));
    strlcpy(cfg.cf_zone, "z'1", sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_records[0], "r<0>", sizeof(cfg.cf_records[0]));
    cfg.cf_record_count = 1;
    f.get("/cloudflare");
    TEST_ASSERT_TRUE(contains(f.out(), "****abcd"));
    TEST_ASSERT_FALSE(contains(f.out(), "segredo"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='z&#39;1'"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='r&lt;0&gt;'"));
}

void test_cloudflare_save_ok_compacta_records() {
    Fixture f;
    f.post("/cloudflare/save", {
        {"token", "NOVO_TOKEN"}, {"zone", " ZONA "}, {"host", "h.com"},
        {"record0", ""}, {"record1", "R1"}, {"record3", "R3"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL_STRING("NOVO_TOKEN", cfg.cf_token);
    TEST_ASSERT_EQUAL_STRING("ZONA", cfg.cf_zone);
    TEST_ASSERT_EQUAL_STRING("h.com", cfg.cf_host);
    TEST_ASSERT_EQUAL(2, cfg.cf_record_count);
    TEST_ASSERT_EQUAL_STRING("R1", cfg.cf_records[0]);
    TEST_ASSERT_EQUAL_STRING("R3", cfg.cf_records[1]);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "NOVO_TOKEN"));
}

void test_cloudflare_save_token_vazio_mantem_atual() {
    Fixture f;
    strlcpy(cfg.cf_token, "ANTIGO", sizeof(cfg.cf_token));
    f.post("/cloudflare/save", {{"zone", "Z"}, {"record0", "R"}});
    TEST_ASSERT_EQUAL_STRING("ANTIGO", cfg.cf_token);
}

void test_cloudflare_save_valor_longo_rejeitado_sem_alterar() {
    Fixture f;
    strlcpy(cfg.cf_zone, "ORIGINAL", sizeof(cfg.cf_zone));
    f.post("/cloudflare/save", {{"zone", "Z"}, {"record2", std::string(40, 'r')}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "muito longo"));
    TEST_ASSERT_EQUAL_STRING("ORIGINAL", cfg.cf_zone);

    const char* fields[] = {"token", "zone", "host"};
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
    TEST_ASSERT_TRUE(contains(f.out(), "color:#ffffff"));
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

void test_iot_pagina_inicial() {
    Fixture f;
    f.get("/iot");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "docs/esp01-pinout.png"));
    TEST_ASSERT_TRUE(contains(f.out(), "[sensor]"));
    TEST_ASSERT_TRUE(contains(f.out(), "[ip]"));
    TEST_ASSERT_TRUE(contains(f.out(), "nenhum"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='-1' selected>Nenhum"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' selected>Alto"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='0' selected>Baixo"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1'><label"));
    TEST_ASSERT_FALSE(contains(f.out(), "http-equiv='refresh'"));
    TEST_ASSERT_FALSE(contains(f.out(), "Cadastre o topico"));
}

void test_iot_pagina_leitura_e_aviso_de_topico() {
    cfg.iot_sensor_pin = 2;
    cfg.iot_module_pin = 0;
    cfg.iot_notify = true;
    Fixture f;
    mock::pinIn[2] = LOW;
    f.iot.begin();
    f.iot.tick(0);
    f.iot.tick(IoTService::DEBOUNCE_MS);
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "http-equiv='refresh' content='5'"));
    TEST_ASSERT_TRUE(contains(f.out(), "baixo"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='2' selected>GPIO2"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' checked>"));
    TEST_ASSERT_TRUE(contains(f.out(), "Cadastre o topico"));

    strlcpy(cfg.ntfy_topic, "topico", sizeof(cfg.ntfy_topic));
    f.get("/iot");
    TEST_ASSERT_FALSE(contains(f.out(), "Cadastre o topico"));
}

void test_iot_save() {
    Fixture f;
    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_high", "0"}, {"module_high", "1"},
                         {"iot_notify", "1"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "IoT configurado"));
    TEST_ASSERT_EQUAL(2, cfg.iot_sensor_pin);
    TEST_ASSERT_EQUAL(0, cfg.iot_module_pin);
    TEST_ASSERT_FALSE(cfg.iot_sensor_high);
    TEST_ASSERT_TRUE(cfg.iot_module_high);
    TEST_ASSERT_TRUE(cfg.iot_notify);
    TEST_ASSERT_EQUAL(INPUT_PULLUP, mock::pinModeV[2]);
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[0]);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"iot_sensor\":2"));

    f.post("/iot/save", {{"sensor", "-1"}, {"module", "-1"},
                         {"sensor_high", "1"}, {"module_high", "0"}});
    TEST_ASSERT_EQUAL(-1, cfg.iot_sensor_pin);
    TEST_ASSERT_FALSE(cfg.iot_notify);
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[0]);
    TEST_ASSERT_EQUAL(LOW, mock::pinOut[0]);
    TEST_ASSERT_FALSE(f.iot.moduleOn());
}

void test_iot_pagina_tipos() {
    Fixture f;
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "value='0' selected>Digital"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='0' selected>Rele"));
    TEST_ASSERT_TRUE(contains(f.out(), ">PIR</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Ultrassom</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Bomba</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Ventilador</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), ">Motor</option>"));
    TEST_ASSERT_TRUE(contains(f.out(), "sk=[0,1,2,3,4,0,0,0,0,0,0,0,0,0,0,5]"));
    TEST_ASSERT_TRUE(contains(f.out(), "mk=[0,1,0,0,0,0,2,2]"));
    TEST_ASSERT_TRUE(contains(f.out(), "}iotSync();"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_pwm' style='display:none'"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_span' style='display:none'"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='0' selected>Automatico"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='0' selected>Seguir o nivel"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='0' selected>Abaixo de"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='threshold' value='0' min='-55' max='100000'>"));
    TEST_ASSERT_FALSE(contains(f.out(), "max='100000'<"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_value' style='display:none'"));
    TEST_ASSERT_TRUE(contains(f.out(), "iotSync"));

    cfg.iot_sensor_type = IoTService::SENSOR_DHT11;
    cfg.iot_module_type = IoTService::MODULE_SERVO;
    cfg.iot_metric = 1;
    cfg.iot_above = true;
    cfg.iot_angle_on = 180;
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' selected>DHT11"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' selected>Servo"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' selected>Temperatura"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' selected>Acima de"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_digital' style='display:none'"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_servo'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='angle_on' value='180'"));

    cfg.iot_sensor_type = IoTService::SENSOR_DHT22;
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "value='2' selected>DHT22"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_metric'>"));

    cfg.iot_sensor_type = IoTService::SENSOR_DS18B20;
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "value='3' selected>DS18B20"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_metric' style='display:none'"));

    cfg.iot_sensor_type = IoTService::SENSOR_PULSE;
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "value='4' selected>Pulsos (10 s)"));

    cfg.iot_sensor_type = IoTService::SENSOR_DIGITAL;
    cfg.iot_module_type = IoTService::MODULE_FAN;
    cfg.iot_manual = IoTService::MANUAL_OFF;
    cfg.iot_toggle = true;
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "value='2' selected>Desligado"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' selected>Alternar"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_pwm'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_span' style='display:none'"));

    cfg.iot_sensor_type = IoTService::SENSOR_RANGE;
    cfg.iot_module_type = IoTService::MODULE_FAN;
    cfg.iot_duty_on = 70;
    cfg.iot_linear = true;
    cfg.iot_threshold = 32767;
    cfg.iot_threshold_hi = 32767;
    cfg.iot_manual = IoTService::MANUAL_ON;
    f.get("/iot");
    TEST_ASSERT_TRUE(contains(f.out(), "value='15' selected>Ultrassom"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='6' selected>Ventilador"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' selected>Ligado"));
    TEST_ASSERT_TRUE(contains(f.out(), "value='1' selected>Proporcional"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_value'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_metric' style='display:none'"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_pwm'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_span'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_level' style='display:none'"));
    TEST_ASSERT_TRUE(contains(f.out(), "id='iot_servo' style='display:none'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='duty_on' value='70'"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='threshold' value='32767' min='-55' max='100000'>"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='threshold_hi' value='32767' min='-55' max='100000'>"));
}

void test_iot_save_dht_servo_e_pulsos() {
    Fixture f;
    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "2"}, {"module_type", "1"},
                         {"metric", "1"}, {"above", "1"},
                         {"threshold", "26"}, {"margin", "2"},
                         {"angle_on", "170"}, {"angle_off", "15"},
                         {"sensor_high", "1"}, {"module_high", "0"},
                         {"iot_notify", "1"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL(2, cfg.iot_sensor_type);
    TEST_ASSERT_EQUAL(1, cfg.iot_module_type);
    TEST_ASSERT_EQUAL(1, cfg.iot_metric);
    TEST_ASSERT_TRUE(cfg.iot_above);
    TEST_ASSERT_EQUAL(26, cfg.iot_threshold);
    TEST_ASSERT_EQUAL(2, cfg.iot_margin);
    TEST_ASSERT_EQUAL(170, cfg.iot_angle_on);
    TEST_ASSERT_EQUAL(15, cfg.iot_angle_off);
    TEST_ASSERT_TRUE(mock::servoAttached);
    TEST_ASSERT_EQUAL(15, mock::servoAngle);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"iot_sensor_type\":2"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "-1"},
                         {"sensor_type", "4"}, {"threshold", "12"},
                         {"sensor_high", "1"}, {"module_high", "0"}});
    TEST_ASSERT_EQUAL(4, cfg.iot_sensor_type);
    TEST_ASSERT_EQUAL(0, cfg.iot_module_type);
    TEST_ASSERT_EQUAL(12, cfg.iot_threshold);
    TEST_ASSERT_EQUAL(0, cfg.iot_margin);
    TEST_ASSERT_FALSE(cfg.iot_above);
    TEST_ASSERT_EQUAL(2, mock::isrPin);
}

void test_iot_save_rejeita_valores() {
    Fixture f;
    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"}, {"sensor_type", "99"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));
    TEST_ASSERT_EQUAL(-1, cfg.iot_sensor_pin);

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "2"}, {"module_type", "99"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "2"}, {"metric", "3"},
                         {"threshold", "10"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "2"}, {"threshold", "x"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "2"}, {"threshold", "40"},
                         {"margin", "99"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"module_type", "1"}, {"angle_on", "200"},
                         {"angle_off", "10"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"module_type", "1"}, {"angle_on", "10"},
                         {"angle_off", "200"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "15"}, {"module_type", "6"},
                         {"threshold", "30"}, {"duty_on", "101"},
                         {"duty_off", "0"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "15"}, {"module_type", "6"},
                         {"threshold", "30"}, {"duty_on", "10"},
                         {"duty_off", "101"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"}, {"manual", "9"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"}, {"toggle", "2"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"}, {"linear", "2"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "15"}, {"module_type", "6"},
                         {"threshold", "30"}, {"linear", "1"},
                         {"threshold_hi", "30"}, {"duty_on", "80"},
                         {"duty_off", "10"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "15"}, {"module_type", "6"},
                         {"threshold", "30"}, {"linear", "1"},
                         {"threshold_hi", "x"}, {"duty_on", "80"},
                         {"duty_off", "10"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Valor invalido"));
}

void test_iot_save_ultrassom_pwm_e_apelido() {
    Fixture f;
    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "15"}, {"module_type", "6"},
                         {"threshold", "30"}, {"margin", "2"}, {"above", "1"},
                         {"duty_on", "70"}, {"duty_off", "5"},
                         {"sensor_high", "1"}, {"module_high", "0"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL(IoTService::SENSOR_RANGE, cfg.iot_sensor_type);
    TEST_ASSERT_EQUAL(IoTService::MODULE_FAN, cfg.iot_module_type);
    TEST_ASSERT_EQUAL(30, cfg.iot_threshold);
    TEST_ASSERT_EQUAL(2, cfg.iot_margin);
    TEST_ASSERT_TRUE(cfg.iot_above);
    TEST_ASSERT_EQUAL(70, cfg.iot_duty_on);
    TEST_ASSERT_EQUAL(5, cfg.iot_duty_off);
    TEST_ASSERT_EQUAL(5 * 1023 / 100, mock::analogValue[0]);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"iot_duty_on\":70"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "5"}, {"module_type", "4"},
                         {"sensor_high", "1"}, {"module_high", "1"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL(IoTService::SENSOR_PIR, cfg.iot_sensor_type);
    TEST_ASSERT_EQUAL(IoTService::MODULE_PUMP, cfg.iot_module_type);
    TEST_ASSERT_EQUAL(100, cfg.iot_duty_on);
    TEST_ASSERT_EQUAL(0, cfg.iot_duty_off);
    TEST_ASSERT_EQUAL(OUTPUT, mock::pinModeV[0]);
    TEST_ASSERT_EQUAL(0, cfg.iot_manual);
    TEST_ASSERT_FALSE(cfg.iot_toggle);
    TEST_ASSERT_FALSE(cfg.iot_linear);

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "14"}, {"module_type", "0"},
                         {"manual", "1"}, {"toggle", "1"},
                         {"sensor_high", "1"}, {"module_high", "1"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_EQUAL(IoTService::SENSOR_BUTTON, cfg.iot_sensor_type);
    TEST_ASSERT_EQUAL(IoTService::MANUAL_ON, cfg.iot_manual);
    TEST_ASSERT_TRUE(cfg.iot_toggle);
    TEST_ASSERT_EQUAL(HIGH, mock::pinOut[0]);

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "15"}, {"module_type", "6"},
                         {"threshold", "10"}, {"threshold_hi", "30"},
                         {"linear", "1"}, {"toggle", "1"},
                         {"duty_on", "100"}, {"duty_off", "0"},
                         {"sensor_high", "1"}, {"module_high", "0"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(cfg.iot_linear);
    TEST_ASSERT_FALSE(cfg.iot_toggle);
    TEST_ASSERT_EQUAL(30, cfg.iot_threshold_hi);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], "\"iot_linear\":true"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"},
                         {"sensor_type", "0"}, {"module_type", "0"},
                         {"linear", "1"}, {"toggle", "0"},
                         {"sensor_high", "1"}, {"module_high", "0"}});
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_FALSE(cfg.iot_linear);
    TEST_ASSERT_FALSE(cfg.iot_toggle);
    TEST_ASSERT_EQUAL(0, cfg.iot_threshold_hi);
}

void test_iot_save_rejeita_pino_e_falha_ao_gravar() {
    Fixture f;
    f.post("/iot/save", {{"sensor", "9"}, {"module", "0"}});
    TEST_ASSERT_EQUAL(400, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Pino invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "x"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Pino invalido"));

    f.post("/iot/save", {{"sensor", "2"}, {"module", "2"}});
    TEST_ASSERT_TRUE(contains(f.out(), "pinos diferentes"));

    MockFS::failWriteOpen = true;
    f.post("/iot/save", {{"sensor", "2"}, {"module", "0"}});
    TEST_ASSERT_TRUE(contains(f.out(), "Falha ao salvar"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_escape);
    RUN_TEST(test_mask_secret);
    RUN_TEST(test_format_duration);
    RUN_TEST(test_format_bytes);
    RUN_TEST(test_begin_registra_rotas_e_gera_csrf_aleatorio);
    RUN_TEST(test_tick_atende_clientes);
    RUN_TEST(test_sem_senha_redireciona_para_definir_senha);
    RUN_TEST(test_autenticacao_obrigatoria);
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
    RUN_TEST(test_log_escapa_conteudo);
    RUN_TEST(test_status_conectado);
    RUN_TEST(test_status_desconectado_e_ota);
    RUN_TEST(test_cloudflare_sem_token);
    RUN_TEST(test_cloudflare_com_token_mascarado_e_valores_escapados);
    RUN_TEST(test_cloudflare_save_ok_compacta_records);
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
    RUN_TEST(test_iot_pagina_inicial);
    RUN_TEST(test_iot_pagina_leitura_e_aviso_de_topico);
    RUN_TEST(test_iot_save);
    RUN_TEST(test_iot_pagina_tipos);
    RUN_TEST(test_iot_save_dht_servo_e_pulsos);
    RUN_TEST(test_iot_save_rejeita_valores);
    RUN_TEST(test_iot_save_ultrassom_pwm_e_apelido);
    RUN_TEST(test_iot_save_rejeita_pino_e_falha_ao_gravar);
    return UNITY_END();
}
