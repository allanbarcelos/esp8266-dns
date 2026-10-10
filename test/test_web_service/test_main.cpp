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
    WebService  web{cfg, logger, store, wifi, ota};
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

// ── inicialização e segurança ───────────────────────────────────────────────

void test_begin_registra_rotas_e_gera_csrf_aleatorio() {
    Fixture f;
    TEST_ASSERT_TRUE(f.srv.started);
    TEST_ASSERT_EQUAL(14, (int)f.srv.routes.size());
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
    const char* paths[] = {"/", "/status", "/log", "/cloudflare", "/ntfy", "/update"};
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
    const char* paths[] = {"/", "/status", "/log", "/cloudflare", "/ntfy", "/password", "/update"};
    for (const char* p : paths) {
        f.get(p);
        TEST_ASSERT_TRUE(f.srv.authRequested);
        TEST_ASSERT_EQUAL(401, f.srv.code);
    }
    const char* posts[] = {"/save", "/cloudflare/save", "/ntfy/save", "/password/save",
                           "/update/auto", "/update/check", "/update/install"};
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
                           "/update/auto", "/update/check", "/update/install"};
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
    TEST_ASSERT_TRUE(contains(f.out(), "512 / 1000 bytes"));
}

void test_root_modo_configuracao_primeiro_acesso() {
    cfg.webpss = "";
    cfg.webusr = "";
    Fixture f;
    f.enterConfigMode();
    f.get("/");
    TEST_ASSERT_EQUAL(200, f.srv.code);
    TEST_ASSERT_TRUE(contains(f.out(), "Modo de configuracao"));
    TEST_ASSERT_TRUE(contains(f.out(), "name='pass1'"));      // define senha do painel
    TEST_ASSERT_TRUE(contains(f.out(), "value='admin'"));
    TEST_ASSERT_TRUE(contains(f.out(), f.web.csrfToken()));
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
    TEST_ASSERT_TRUE(contains(f.out(), "Heap livre"));
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_escape);
    RUN_TEST(test_mask_secret);
    RUN_TEST(test_format_duration);
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
    return UNITY_END();
}
