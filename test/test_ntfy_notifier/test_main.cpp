// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Clock.h"
#include "Config.h"
#include "NtfyNotifier.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    strlcpy(cfg.ntfy_topic, "meu-topico", sizeof(cfg.ntfy_topic));
}
void tearDown() {}

struct Fix {
    Clock            clock{fakeTime};
    PublicIPResolver resolver{logger, clock};
    NtfyNotifier     ntfy{cfg, logger, resolver, clock};
};

void test_sem_envio_sem_topico() {
    cfg.ntfy_topic[0] = '\0';
    Fix f;
    f.ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_sem_relogio_nao_envia() {
    fakeNow = 1000;
    Fix f;
    f.ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "relogio nao sincronizado"));
    TEST_ASSERT_EQUAL_STRING("", cfg.publicIP);
}

void test_envia_no_boot_sem_cloudflare() {
    TEST_ASSERT_EQUAL_STRING("", cfg.cf_token);
    strlcpy(cfg.publicIP, "9.9.9.9", sizeof(cfg.publicIP));
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(0);

    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("https://api4.ipify.org", MockHTTP::requests[0].url.c_str());
    TEST_ASSERT_EQUAL_STRING("GET", MockHTTP::requests[0].method.c_str());
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/meu-topico", MockHTTP::requests[1].url.c_str());
    TEST_ASSERT_EQUAL_STRING("POST", MockHTTP::requests[1].method.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[1].payload, "[ip] 1.2.3.4"));
    TEST_ASSERT_FALSE(contains(MockHTTP::requests[1].payload, "9.9.9.9"));
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", cfg.publicIP);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy enviado (200)"));
    for (auto& r : MockHTTP::requests) {
        TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, r.trustPem);
        TEST_ASSERT_EQUAL((long)fakeNow, r.x509Time);
        TEST_ASSERT_FALSE(r.insecure);
    }
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
}

void test_nao_reenvia_mesmo_ip() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(0);
    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(3, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("GET", MockHTTP::requests[2].method.c_str());
}

void test_reenvia_quando_ip_muda() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(0);
    MockHTTP::push(200, "5.6.7.8");
    f.ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(4, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("POST", MockHTTP::requests[3].method.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[3].payload, "5.6.7.8"));
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", cfg.publicIP);
}

void test_falha_aguarda_intervalo_para_retentar() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(500);
    f.ntfy.tick(1000);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy erro: 500"));

    f.ntfy.tick(1000 + NtfyNotifier::RETRY_INTERVAL - 1);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());

    MockHTTP::push(200, "1.2.3.4");
    f.ntfy.tick(1000 + NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_EQUAL(4, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy enviado (200)"));
}

void test_erro_de_conexao() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::beginResults = {true, false};
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: falha ao conectar"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("GET", MockHTTP::requests[0].method.c_str());
}

void test_codigo_negativo() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(-1);
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy erro: -1"));
}

void test_memoria_insuficiente_nao_envia() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    char ip[16];
    TEST_ASSERT_TRUE(f.resolver.resolve(ip, sizeof(ip), 0));
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: memoria insuficiente"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_tls_indisponivel_nao_envia() {
    Fix f;
    MockHTTP::push(200, "1.2.3.4");
    char ip[16];
    TEST_ASSERT_TRUE(f.resolver.resolve(ip, sizeof(ip), 0));
    MockTLS::nullOwned = true;
    MockTLS::bufIn = 0;
    f.ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: TLS indisponivel"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(0, MockTLS::bufIn);
}

void test_iot_sem_leitura_ainda_envia_ip() {
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, "ok");
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[1].payload, "[ip] 1.2.3.4"));
}

void test_sensor_no_mesmo_topico() {
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    strlcpy(cfg.iot.sensor, "digital", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    cfg.iot.notify = true;
    mock::pinIn[2] = HIGH;
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(iot.pending());

    MockHTTP::push(200, "ok");
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("POST", MockHTTP::requests[0].method.c_str());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "[sensor] ativo"));
    TEST_ASSERT_FALSE(iot.pending());

    mock::pinIn[2] = LOW;
    iot.tick(IoTService::DEBOUNCE_MS + 1);
    iot.tick(IoTService::DEBOUNCE_MS + 1 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(iot.pending());
    ntfy.tick(IoTService::DEBOUNCE_MS + 1 + IoTService::DEBOUNCE_MS);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());

    MockHTTP::push(500, "x");
    ntfy.tick(NtfyNotifier::RETRY_INTERVAL);
    TEST_ASSERT_TRUE(iot.pending());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy erro: 500"));
}

void test_valida_topico() {
    TEST_ASSERT_TRUE(NtfyNotifier::isValidTopic("abc_DEF-123"));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic(""));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic(String(std::string(64, 'a'))));
    TEST_ASSERT_TRUE(NtfyNotifier::isValidTopic(String(std::string(63, 'a'))));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic("a/b"));
    TEST_ASSERT_FALSE(NtfyNotifier::isValidTopic("a b"));
}

// ── Comandos pelo ntfy (fluxo contínuo) ──────────────────────────────────────

static const char* OK_HDR = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n\r\n";

struct Cmd {
    Clock            clock{fakeTime};
    PublicIPResolver resolver{logger, clock};
    IoTService       iot{cfg, logger};
    NtfyNotifier     ntfy{cfg, logger, resolver, clock, &iot};

    Cmd() {
        cfg.ntfy_topic[0] = '\0';               // só comandos: nada a publicar
        strlcpy(cfg.ntfy_cmd, "cmd-abc", sizeof(cfg.ntfy_cmd));
        strlcpy(cfg.iot.actuator, "rele", sizeof(cfg.iot.actuator));
        cfg.iot.actuatorPin = 0;
        iot.begin();
        MockHTTP::keepOpen = true;
    }

    // Conecta e lê status e cabeçalhos
    void open(unsigned long t = 0) {
        MockHTTP::rawHttp = OK_HDR;
        ntfy.tick(t);
        ntfy.tick(t + 1);
    }

    void send(const std::string& line) { MockTLS::live->inject(line + "\r\n"); }
};

static std::string message(const char* id, const char* text, long age = 0) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%s\",\"time\":%ld,\"event\":\"message\",\"topic\":\"cmd-abc\","
             "\"message\":\"%s\"}", id, (long)fakeNow - age, text);
    return buf;
}

void test_comandos_conecta_validando_o_certificado() {
    Cmd c;
    TEST_ASSERT_EQUAL_STRING("conectando", c.ntfy.commandStatus());
    MockHTTP::rawHttp = OK_HDR;
    c.ntfy.tick(0);
    TEST_ASSERT_TRUE(contains(MockHTTP::lastRaw,
        "GET /cmd-abc/json HTTP/1.0\r\nHost: ntfy.sh\r\nUser-Agent: ESP8266\r\n\r\n"));
    TEST_ASSERT_FALSE(contains(MockHTTP::lastRaw, "Authorization"));
    TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, MockTLS::lastTrustPem);
    TEST_ASSERT_EQUAL((long)fakeNow, (long)MockTLS::lastTime);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
    TEST_ASSERT_EQUAL(NtfyNotifier::CONNECT_MS, MockTLS::lastTimeoutMs);

    c.ntfy.tick(1);
    TEST_ASSERT_EQUAL_STRING("conectado", c.ntfy.commandStatus());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: comandos conectados"));

    c.send("{\"id\":\"o1\",\"time\":1,\"event\":\"open\",\"topic\":\"cmd-abc\"}");
    c.send("{\"event\":\"keepalive\"}");
    c.send("lixo");
    c.send(message("m1", "ligar"));
    c.ntfy.tick(2);
    TEST_ASSERT_TRUE(c.iot.actuatorOn());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: comando \"ligar\""));

    c.send(message("m2", "DESLIGAR"));
    c.ntfy.tick(3);
    TEST_ASSERT_FALSE(c.iot.actuatorOn());
}

void test_comandos_reconecta_pedindo_o_que_perdeu() {
    Cmd c;
    c.open();
    c.send(message("m7", "ligar"));
    c.ntfy.tick(10);

    // Servidor fechou: espera e reabre com since=<último id>
    MockHTTP::keepOpen = false;
    c.ntfy.tick(20);
    TEST_ASSERT_TRUE(logHas(logger, "fluxo de comandos encerrado"));
    MockHTTP::keepOpen = true;
    MockHTTP::rawHttp = OK_HDR;
    c.ntfy.tick(20 + NtfyNotifier::RECONNECT_MS - 1);
    TEST_ASSERT_EQUAL_STRING("conectando", c.ntfy.commandStatus());
    TEST_ASSERT_FALSE(contains(MockHTTP::lastRaw, "since"));
    c.ntfy.tick(20 + NtfyNotifier::RECONNECT_MS);
    TEST_ASSERT_TRUE(contains(MockHTTP::lastRaw, "GET /cmd-abc/json?since=m7 HTTP/1.0"));

    // Sem nada (nem keepalive) por muito tempo: reconecta
    unsigned long t = 20 + NtfyNotifier::RECONNECT_MS;
    c.ntfy.tick(t + 1);
    c.ntfy.tick(t + 1 + NtfyNotifier::IDLE_TIMEOUT);
    TEST_ASSERT_EQUAL_STRING("conectado", c.ntfy.commandStatus());
    c.ntfy.tick(t + 2 + NtfyNotifier::IDLE_TIMEOUT);
    TEST_ASSERT_TRUE(logHas(logger, "fluxo de comandos parado"));
}

void test_comandos_com_token_e_resposta_de_erro() {
    strlcpy(cfg.ntfy_token, "tk_segredo", sizeof(cfg.ntfy_token));
    Cmd c;
    c.open();
    TEST_ASSERT_TRUE(contains(MockHTTP::lastRaw, "Authorization: Bearer tk_segredo\r\n"));
}

void test_comandos_invalidos_respondem_no_topico_de_dados() {
    Cmd c;
    c.open();
    c.send(message("e1", "voar"));
    c.ntfy.tick(10);
    TEST_ASSERT_TRUE(logHas(logger, "[erro] comando invalido: voar"));
    c.send(message("e2", "50%"));
    c.ntfy.tick(11);
    TEST_ASSERT_TRUE(logHas(logger, "[erro] comando nao vale para este atuador: 50%"));

    // Com tópico de dados, a resposta sai e o fluxo fecha para o envio
    strlcpy(cfg.ntfy_topic, "dados", sizeof(cfg.ntfy_topic));
    strlcpy(cfg.ntfy_token, "tk_x", sizeof(cfg.ntfy_token));
    MockHTTP::push(200, "ok");
    c.ntfy.tick(12);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "[erro] comando nao vale"));
    bool auth = false;
    for (auto& h : MockHTTP::requests[0].headers)
        auth = auth || (h.first == "Authorization" && h.second == "Bearer tk_x");
    TEST_ASSERT_TRUE(auth);
    TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, MockHTTP::requests[0].trustPem);
    TEST_ASSERT_EQUAL((long)fakeNow, MockHTTP::requests[0].x509Time);
    TEST_ASSERT_FALSE(MockHTTP::requests[0].insecure);
    TEST_ASSERT_FALSE(MockTLS::live && MockTLS::live->isConnected());

    // Resposta que falha tenta de novo depois do intervalo curto
    MockHTTP::rawHttp = OK_HDR;
    c.ntfy.tick(13);                                 // reconecta
    c.ntfy.tick(14);
    c.send(message("e3", "voar"));
    c.ntfy.tick(15);
    MockHTTP::push(500, "x");
    c.ntfy.tick(12 + NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
}

void test_comandos_ignorados_quando_velhos_ou_longos() {
    Cmd c;
    c.open();
    c.send(message("v1", "ligar", NtfyNotifier::STALE_S + 1));
    c.ntfy.tick(10);
    TEST_ASSERT_FALSE(c.iot.actuatorOn());
    TEST_ASSERT_TRUE(logHas(logger, "comando antigo ignorado"));

    // Linha maior que o buffer (anexo, título): descartada inteira
    MockTLS::live->inject(std::string(NtfyNotifier::LINE_LEN + 50, 'x') + "\n");
    c.send(message("v2", "ligar"));
    c.ntfy.tick(11);                                 // lê até 256 bytes por tick
    TEST_ASSERT_FALSE(c.iot.actuatorOn());
    c.ntfy.tick(12);
    c.ntfy.tick(13);
    TEST_ASSERT_TRUE(c.iot.actuatorOn());
}

void test_comandos_falhas_esperam_cada_vez_mais() {
    Cmd c;
    MockHTTP::rawHttp = "HTTP/1.1 401 Unauthorized\r\n\r\n";
    c.ntfy.tick(0);
    c.ntfy.tick(1);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: comandos HTTP 401"));
    TEST_ASSERT_EQUAL_STRING("conectando", c.ntfy.commandStatus());

    MockHTTP::rawHttp = "lixo\r\n\r\n";
    c.ntfy.tick(1 + 4999);                           // backoff de 5 s
    TEST_ASSERT_FALSE(contains(MockHTTP::lastRaw, "lixo"));
    int calls = MockTLS::connectCalls;
    c.ntfy.tick(1 + 5000);
    TEST_ASSERT_EQUAL(calls + 1, MockTLS::connectCalls);
    c.ntfy.tick(1 + 5001);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: comandos HTTP 0"));

    MockTLS::failConnect = true;                     // depois 10 s
    c.ntfy.tick(5002 + 9999);
    TEST_ASSERT_EQUAL(calls + 1, MockTLS::connectCalls);
    c.ntfy.tick(5002 + 10000);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: sem conexao para comandos"));

    // O teto é 5 min
    unsigned long t = 5002 + 10000;
    for (int i = 0; i < 8; i++) {
        t += NtfyNotifier::BACKOFF_MAX_S * 1000UL;
        c.ntfy.tick(t);
    }
    calls = MockTLS::connectCalls;
    c.ntfy.tick(t + NtfyNotifier::BACKOFF_MAX_S * 1000UL);
    TEST_ASSERT_EQUAL(calls + 1, MockTLS::connectCalls);

    // Conectar zera a espera
    MockTLS::failConnect = false;
    t += NtfyNotifier::BACKOFF_MAX_S * 2000UL;
    c.open(t);
    TEST_ASSERT_EQUAL_STRING("conectado", c.ntfy.commandStatus());
}

void test_comandos_sem_memoria_sem_tls_e_sem_relogio() {
    {
        Cmd c;
        ESP._heap = 1000;
        c.ntfy.tick(0);
        TEST_ASSERT_TRUE(logHas(logger, "memoria insuficiente para comandos"));
        TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    }
    resetMocks();
    {
        MockTLS::nullOwned = true;                   // contexto TLS sem memória
        Cmd c;
        c.ntfy.tick(0);
        TEST_ASSERT_TRUE(logHas(logger, "sem conexao para comandos"));
        TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    }
    resetMocks();
    {
        fakeNow = 1000;                               // relógio ainda em 1970
        Cmd c;
        c.ntfy.tick(0);
        TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
        fakeNow = Clock::MIN_VALID_EPOCH + 100;
        MockHTTP::rawHttp = OK_HDR;
        c.ntfy.tick(NtfyNotifier::RECONNECT_MS - 1);
        TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
        c.ntfy.tick(NtfyNotifier::RECONNECT_MS);
        TEST_ASSERT_EQUAL(1, MockTLS::connectCalls);
    }
}

void test_comandos_pausam_e_desligam() {
    Cmd c;
    c.open();
    TEST_ASSERT_TRUE(MockTLS::live->isConnected());

    // DNS ou OTA vão usar a rede: solta a sessão na hora
    c.ntfy.hold(true);
    TEST_ASSERT_EQUAL_STRING("pausado", c.ntfy.commandStatus());
    TEST_ASSERT_FALSE(MockTLS::live && MockTLS::live->isConnected());
    int calls = MockTLS::connectCalls;
    c.ntfy.tick(100);
    TEST_ASSERT_EQUAL(calls, MockTLS::connectCalls);
    c.ntfy.hold(false);
    c.open(200);                                      // volta sem esperar
    TEST_ASSERT_EQUAL(calls + 1, MockTLS::connectCalls);

    WiFi._status = WL_DISCONNECTED;
    c.ntfy.tick(300);
    TEST_ASSERT_FALSE(MockTLS::live && MockTLS::live->isConnected());
    WiFi._status = WL_CONNECTED;

    // Sem atuador ou sem tópico: desligado
    cfg.ntfy_cmd[0] = '\0';
    c.ntfy.tick(400);
    TEST_ASSERT_EQUAL_STRING("desligado", c.ntfy.commandStatus());
    strlcpy(cfg.ntfy_cmd, "cmd-abc", sizeof(cfg.ntfy_cmd));
    cfg.iot.actuatorPin = -1;
    c.iot.begin();
    TEST_ASSERT_EQUAL_STRING("desligado", c.ntfy.commandStatus());
    NtfyNotifier semIot{cfg, logger, c.resolver, c.clock};
    TEST_ASSERT_EQUAL_STRING("desligado", semIot.commandStatus());
}

// Evento sai em segundos; leitura periódica, por minuto
void test_eventos_saem_rapido_leituras_periodicas_nao() {
    Fix f;
    strlcpy(cfg.iot.actuator, "rele", sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    iot.begin();
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(200, "ok");
    ntfy.tick(0);                                     // IP no boot
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());

    iot.command("ligar", 100);
    TEST_ASSERT_TRUE(iot.urgent());
    ntfy.tick(NtfyNotifier::POST_GAP - 1);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    MockHTTP::push(200, "ok");
    ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(3, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[2].payload, "[atuador] ligado"));
    TEST_ASSERT_FALSE(iot.urgent());
}

// Leitura periódica (sem mudança) espera o intervalo longo
void test_leitura_periodica_nao_e_urgente() {
    Fix f;
    strlcpy(cfg.iot.sensor, "pulsos", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    cfg.iot.notify = true;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::PULSE_WINDOW_MS);            // 1ª leitura: urgente
    TEST_ASSERT_TRUE(iot.urgent());
    MockHTTP::push(200, "ok");
    ntfy.tick(IoTService::PULSE_WINDOW_MS);
    TEST_ASSERT_FALSE(iot.pending());

    iot.tick(IoTService::PULSE_WINDOW_MS * 2);        // igual: periódica
    TEST_ASSERT_TRUE(iot.pending());
    TEST_ASSERT_FALSE(iot.urgent());
    ntfy.tick(IoTService::PULSE_WINDOW_MS * 2);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
}

// ── Botões de ação na notificação ────────────────────────────────────────────

static std::string header(const MockHTTP::Request& r, const char* name) {
    for (auto& h : r.headers)
        if (h.first == name) return h.second;
    return "";
}

// Publica a mudança de um atuador e devolve o cabeçalho Actions
static std::string actionsFor(const char* actuator, const char* sensor = "") {
    resetMocks();
    logger = Logger();
    strlcpy(cfg.iot.actuator, actuator, sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = 0;
    strlcpy(cfg.iot.sensor, sensor, sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = sensor[0] ? 2 : -1;
    cfg.iot.notify = true;
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    iot.begin();
    iot.command("ligar", 0);
    MockHTTP::push(200, "ok");
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, MockHTTP::requests[0].trustPem);
    TEST_ASSERT_FALSE(MockHTTP::requests[0].insecure);
    return header(MockHTTP::requests[0], "Actions");
}

void test_acoes_pelo_que_o_atuador_faz() {
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));
    TEST_ASSERT_EQUAL_STRING(
        "http, Ligar, https://ntfy.sh/cmd-x, method=POST, body=ligar, clear=true; "
        "http, Desligar, https://ntfy.sh/cmd-x, method=POST, body=desligar, clear=true; "
        "http, Pulso, https://ntfy.sh/cmd-x, method=POST, body=pulso 1, clear=true",
        actionsFor("rele").c_str());

    std::string level = actionsFor("ventilador");
    TEST_ASSERT_TRUE(contains(level, "http, Desligar,"));
    TEST_ASSERT_TRUE(contains(level, "http, 50%, https://ntfy.sh/cmd-x, method=POST, body=50%"));
    TEST_ASSERT_TRUE(contains(level, "http, 100%,"));

    std::string servo = actionsFor("servo");
    TEST_ASSERT_TRUE(contains(servo, "http, Abrir,"));
    TEST_ASSERT_TRUE(contains(servo, "body=fechar"));
    TEST_ASSERT_TRUE(contains(servo, "body=pulso 3"));

    TEST_ASSERT_TRUE(contains(actionsFor("fita"), "body=pulso 5"));

    // Com a regra, o terceiro botão volta ao automático
    std::string rule = actionsFor("rele", "pir");
    TEST_ASSERT_TRUE(contains(rule, "http, Auto, https://ntfy.sh/cmd-x, method=POST, body=auto"));
    TEST_ASSERT_FALSE(contains(rule, "Pulso"));
}

void test_acoes_com_token_e_sem_topico_de_comandos() {
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));
    strlcpy(cfg.ntfy_token, "tk_abc", sizeof(cfg.ntfy_token));
    std::string a = actionsFor("rele");
    TEST_ASSERT_TRUE(contains(a, "clear=true, headers.Authorization=Bearer tk_abc; http, Desligar"));

    cfg.ntfy_cmd[0] = '\0';
    TEST_ASSERT_EQUAL_STRING("", actionsFor("rele").c_str());
}

void test_acoes_so_com_atuador() {
    strlcpy(cfg.ntfy_cmd, "cmd-x", sizeof(cfg.ntfy_cmd));
    strlcpy(cfg.iot.sensor, "pir", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    cfg.iot.notify = true;
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    mock::pinIn[2] = HIGH;
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    MockHTTP::push(200, "ok");
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_STRING("", header(MockHTTP::requests[0], "Actions").c_str());

    // IP e respostas de erro nunca levam botões
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].payload, "[sensor] presenca"));
}

// ── Envio valida o certificado ───────────────────────────────────────────────

// Evento do IoT: só o POST, sem busca de IP antes
static void eventoDoSensor(IoTService& iot) {
    strlcpy(cfg.iot.sensor, "digital", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    cfg.iot.notify = true;
    mock::pinIn[2] = HIGH;
    iot.begin();
    iot.tick(0);
    iot.tick(IoTService::DEBOUNCE_MS);
    TEST_ASSERT_TRUE(iot.pending());
}

// O envio leva o token: a conexão precisa validar o certificado
void test_envio_valida_o_certificado() {
    strlcpy(cfg.ntfy_token, "tk_segredo", sizeof(cfg.ntfy_token));
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    eventoDoSensor(iot);
    MockHTTP::push(200, "ok");
    ntfy.tick(0);

    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    const MockHTTP::Request& r = MockHTTP::requests[0];
    TEST_ASSERT_EQUAL_STRING("POST", r.method.c_str());
    TEST_ASSERT_EQUAL_STRING("Bearer tk_segredo", header(r, "Authorization").c_str());
    TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, r.trustPem);
    TEST_ASSERT_EQUAL((long)fakeNow, r.x509Time);
    TEST_ASSERT_FALSE(r.insecure);
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
    TEST_ASSERT_EQUAL(5000UL, MockTLS::lastTimeoutMs);
    TEST_ASSERT_FALSE(iot.pending());
}

// Relógio em 1970: o BearSSL recusaria o certificado. Nem tenta; o evento
// fica para depois e sai quando o relógio sincroniza.
void test_envio_espera_o_relogio() {
    fakeNow = 1000;
    strlcpy(cfg.ntfy_token, "tk_segredo", sizeof(cfg.ntfy_token));
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    eventoDoSensor(iot);
    ntfy.tick(0);

    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL(0, MockTLS::connectCalls);
    TEST_ASSERT_EQUAL(0, MockTLS::bufIn);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: relogio nao sincronizado"));
    TEST_ASSERT_TRUE(iot.pending());

    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    ntfy.tick(NtfyNotifier::POST_GAP - 1);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    MockHTTP::push(200, "ok");
    ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
    TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, MockHTTP::requests[0].trustPem);
    TEST_ASSERT_EQUAL((long)fakeNow, MockHTTP::requests[0].x509Time);
    TEST_ASSERT_FALSE(iot.pending());
}

// No limite: MIN_VALID_EPOCH ainda não conta como sincronizado
void test_envio_relogio_no_limite() {
    fakeNow = Clock::MIN_VALID_EPOCH;
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    eventoDoSensor(iot);
    ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: relogio nao sincronizado"));

    fakeNow = Clock::MIN_VALID_EPOCH + 1;
    MockHTTP::push(200, "ok");
    ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());
}

// Certificado recusado (intermediário, CA trocada): o handshake falha, o
// HTTPClient devolve código negativo e o evento fica para a próxima vez
void test_envio_certificado_recusado_tenta_de_novo() {
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    eventoDoSensor(iot);
    MockHTTP::push(-1);
    ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy erro: -1"));
    TEST_ASSERT_TRUE(iot.pending());

    MockHTTP::push(200, "ok");
    ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());
    TEST_ASSERT_FALSE(iot.pending());
    for (auto& r : MockHTTP::requests) TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, r.trustPem);
}

// Memória curta: nem relógio nem TLS são consultados
void test_envio_sem_memoria_antes_do_relogio() {
    fakeNow = 1000;
    Fix f;
    IoTService iot{cfg, logger};
    NtfyNotifier ntfy{cfg, logger, f.resolver, f.clock, &iot};
    eventoDoSensor(iot);
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    ntfy.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "ntfy: memoria insuficiente"));
    TEST_ASSERT_FALSE(logHas(logger, "ntfy: relogio nao sincronizado"));
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

// Nenhuma conexão do ntfy, em nenhum caminho, desliga a validação
void test_nenhum_caminho_usa_tls_sem_validacao() {
    strlcpy(cfg.ntfy_token, "tk_x", sizeof(cfg.ntfy_token));
    Cmd c;
    strlcpy(cfg.ntfy_topic, "dados", sizeof(cfg.ntfy_topic));
    cfg.iot.notify = true;
    c.iot.begin();                                     // o IoT copia a config no begin
    MockHTTP::push(200, "1.2.3.4");                   // IP no boot
    MockHTTP::push(200, "ok");
    c.ntfy.tick(0);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::requests.size());

    c.open(1);                                         // fluxo de comandos
    TEST_ASSERT_EQUAL_STRING("conectado", c.ntfy.commandStatus());
    TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, MockTLS::live->trustPem);
    TEST_ASSERT_FALSE(MockTLS::live->insecure);
    c.send(message("z1", "voar"));                    // resposta de erro
    c.ntfy.tick(3);
    MockHTTP::push(200, "ok");
    c.ntfy.tick(NtfyNotifier::POST_GAP);
    TEST_ASSERT_EQUAL(3, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[2].payload, "[erro]"));

    c.iot.command("ligar", NtfyNotifier::POST_GAP);    // mudança do atuador
    MockHTTP::push(200, "ok");
    c.ntfy.tick(NtfyNotifier::POST_GAP * 2);
    TEST_ASSERT_EQUAL(4, (int)MockHTTP::requests.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[3].payload, "[atuador] ligado"));
    TEST_ASSERT_FALSE(header(MockHTTP::requests[3], "Actions").empty());

    for (auto& r : MockHTTP::requests) {
        TEST_ASSERT_EQUAL_PTR(CLOUDFLARE_CA, r.trustPem);
        TEST_ASSERT_EQUAL((long)fakeNow, r.x509Time);
        TEST_ASSERT_FALSE(r.insecure);
    }
    TEST_ASSERT_EQUAL(0, MockTLS::insecureCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sem_envio_sem_topico);
    RUN_TEST(test_sem_relogio_nao_envia);
    RUN_TEST(test_envia_no_boot_sem_cloudflare);
    RUN_TEST(test_nao_reenvia_mesmo_ip);
    RUN_TEST(test_reenvia_quando_ip_muda);
    RUN_TEST(test_falha_aguarda_intervalo_para_retentar);
    RUN_TEST(test_erro_de_conexao);
    RUN_TEST(test_codigo_negativo);
    RUN_TEST(test_iot_sem_leitura_ainda_envia_ip);
    RUN_TEST(test_sensor_no_mesmo_topico);
    RUN_TEST(test_memoria_insuficiente_nao_envia);
    RUN_TEST(test_tls_indisponivel_nao_envia);
    RUN_TEST(test_valida_topico);
    RUN_TEST(test_comandos_conecta_validando_o_certificado);
    RUN_TEST(test_comandos_reconecta_pedindo_o_que_perdeu);
    RUN_TEST(test_comandos_com_token_e_resposta_de_erro);
    RUN_TEST(test_comandos_invalidos_respondem_no_topico_de_dados);
    RUN_TEST(test_comandos_ignorados_quando_velhos_ou_longos);
    RUN_TEST(test_comandos_falhas_esperam_cada_vez_mais);
    RUN_TEST(test_comandos_sem_memoria_sem_tls_e_sem_relogio);
    RUN_TEST(test_comandos_pausam_e_desligam);
    RUN_TEST(test_eventos_saem_rapido_leituras_periodicas_nao);
    RUN_TEST(test_leitura_periodica_nao_e_urgente);
    RUN_TEST(test_acoes_pelo_que_o_atuador_faz);
    RUN_TEST(test_acoes_com_token_e_sem_topico_de_comandos);
    RUN_TEST(test_acoes_so_com_atuador);
    RUN_TEST(test_envio_valida_o_certificado);
    RUN_TEST(test_envio_espera_o_relogio);
    RUN_TEST(test_envio_relogio_no_limite);
    RUN_TEST(test_envio_certificado_recusado_tenta_de_novo);
    RUN_TEST(test_envio_sem_memoria_antes_do_relogio);
    RUN_TEST(test_nenhum_caminho_usa_tls_sem_validacao);
    return UNITY_END();
}
