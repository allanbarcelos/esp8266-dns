// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "HttpServer.h"
#include "../web_browser.h"

// Handler dos testes: guarda o pedido e responde conforme o caminho
struct Probe {
    HttpServer  srv{80};
    MockBrowser b{[this]() { srv.handleClient(); }};
    int         calls = 0;
    uint8_t     method = 0;
    std::string path, auth, plain;

    std::function<void(Probe&)> reply = [](Probe& p) { p.srv.send(200, "text/plain", "ok"); };

    Probe() { srv.begin(&Probe::handle, this); }

    static void handle(void* self) {
        Probe& p = *static_cast<Probe*>(self);
        p.calls++;
        p.method = p.srv.method();
        p.path = p.srv.path();
        p.auth = p.srv.authorization().c_str();
        p.plain = p.srv.arg("plain").c_str();
        p.reply(p);
    }

    std::string arg(const char* n) { return srv.arg(n).c_str(); }

    // Conexão que fica na fila, com o que o navegador já mandou
    std::shared_ptr<MockConn> connect(const std::string& rx = "") {
        auto c = std::make_shared<MockConn>();
        c->rx = rx;
        MockServer::pending.push_back(c);
        return c;
    }
};

void setUp()    { resetMocks(); }
void tearDown() {}

// ── pedido ───────────────────────────────────────────────────────────────────

void test_get_com_query_e_resposta_com_tamanho() {
    Probe p;
    TEST_ASSERT_TRUE(MockServer::begun);
    p.b.send("GET /x/y?a=1&b=dois+tres&c=%41%62%2f HTTP/1.1\r\nHost: e\r\n\r\n");
    TEST_ASSERT_EQUAL(1, p.calls);
    TEST_ASSERT_EQUAL(HttpServer::GET, p.method);
    TEST_ASSERT_EQUAL_STRING("/x/y", p.path.c_str());
    TEST_ASSERT_EQUAL_STRING("1", p.arg("a").c_str());
    TEST_ASSERT_EQUAL_STRING("dois tres", p.arg("b").c_str());
    TEST_ASSERT_EQUAL_STRING("Ab/", p.arg("c").c_str());
    TEST_ASSERT_EQUAL_STRING("", p.arg("d").c_str());
    TEST_ASSERT_EQUAL_STRING("", p.plain.c_str());

    TEST_ASSERT_EQUAL(200, p.b.code);
    TEST_ASSERT_EQUAL_STRING("text/plain", p.b.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("2", p.b.headers["Content-Length"].c_str());
    TEST_ASSERT_EQUAL_STRING("close", p.b.headers["Connection"].c_str());
    TEST_ASSERT_EQUAL_STRING("ok", p.b.out.c_str());
    TEST_ASSERT_TRUE(p.b.conn->closed);
}

// Como o core: query primeiro, corpo urlencoded depois; o corpo cru em plain
void test_post_urlencoded_e_plain() {
    Probe p;
    p.b.send("POST /s?a=q HTTP/1.1\r\ncontent-type: application/x-www-form-urlencoded; charset=UTF-8\r\n"
             "CONTENT-LENGTH: 19\r\n\r\na=b&ligar&nome=x%20");
    TEST_ASSERT_EQUAL(HttpServer::POST, p.method);
    TEST_ASSERT_EQUAL_STRING("q", p.arg("a").c_str());
    TEST_ASSERT_EQUAL_STRING("", p.arg("ligar").c_str());
    TEST_ASSERT_EQUAL_STRING("x ", p.arg("nome").c_str());
    TEST_ASSERT_EQUAL_STRING("a=b&ligar&nome=x%20", p.plain.c_str());
}

// Outro tipo de corpo (JSON, texto): só em plain
void test_corpo_que_nao_e_formulario() {
    Probe p;
    p.b.send("POST /s HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 7\r\n\r\ncmd=abc");
    TEST_ASSERT_EQUAL_STRING("", p.arg("cmd").c_str());
    TEST_ASSERT_EQUAL_STRING("cmd=abc", p.plain.c_str());
}

void test_decodificacao_e_nomes_parecidos() {
    Probe p;
    p.b.send("GET /?ab=1&a&a=2&x=%zz%4z%4&y=%&z=50%25 HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("", p.arg("a").c_str());          // "a" sem valor vem antes
    TEST_ASSERT_EQUAL_STRING("1", p.arg("ab").c_str());
    TEST_ASSERT_EQUAL_STRING("%zz%4z%4", p.arg("x").c_str());     // % sem dois hex fica
    TEST_ASSERT_EQUAL_STRING("%", p.arg("y").c_str());
    TEST_ASSERT_EQUAL_STRING("50%", p.arg("z").c_str());
    TEST_ASSERT_EQUAL_STRING("", p.arg("abc").c_str());
}

// Cabeçalho sem dois-pontos é ignorado; Authorization longo é cortado
void test_cabecalhos() {
    Probe p;
    std::string longAuth(HttpServer::MAX_AUTH + 100, 'k');
    p.b.send("GET / HTTP/1.1\r\nlixo\r\nAuthorization:   Basic abc\r\nX-Outro: 1\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("Basic abc", p.auth.c_str());
    p.b.send("GET / HTTP/1.1\nAuthorization: " + longAuth + "\n\n");   // só \n também vale
    TEST_ASSERT_EQUAL(2, p.calls);
    TEST_ASSERT_TRUE(p.auth.size() > HttpServer::MAX_AUTH);
    TEST_ASSERT_TRUE(p.auth.size() < longAuth.size());
    p.b.send("GET / HTTP/1.1\r\n\r\n");                                 // não vaza entre pedidos
    TEST_ASSERT_EQUAL_STRING("", p.auth.c_str());
}

void test_metodo_desconhecido_chega_ao_handler() {
    Probe p;
    p.b.send("DELETE /x HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL(HttpServer::OTHER, p.method);
}

// ── pedidos recusados antes do handler ───────────────────────────────────────

void test_pedido_malformado() {
    Probe p;
    const char* bad[] = {"GET\r\n\r\n", "GET /\r\n\r\n", "lixo"};
    for (const char* r : bad) {
        p.b.send(r);
        TEST_ASSERT_EQUAL(400, p.b.code);
    }
    // Cabeçalhos que não terminam
    p.b.send("GET / HTTP/1.1\r\nHost: e\r\n");
    TEST_ASSERT_EQUAL(400, p.b.code);
    TEST_ASSERT_EQUAL(0, p.calls);
}

void test_caminho_longo_demais() {
    Probe p;
    p.b.send("GET /" + std::string(sizeof("/update/install") + 10, 'a') + " HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL(404, p.b.code);
    TEST_ASSERT_EQUAL(0, p.calls);
}

void test_corpo_grande_demais_ou_negativo() {
    Probe p;
    p.b.send("POST / HTTP/1.1\r\nContent-Length: " + std::to_string(HttpServer::MAX_BODY + 1) + "\r\n\r\n");
    TEST_ASSERT_EQUAL(413, p.b.code);
    p.b.send("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n");
    TEST_ASSERT_EQUAL(413, p.b.code);
    TEST_ASSERT_EQUAL(0, p.calls);

    p.b.send("POST / HTTP/1.1\r\nContent-Length: " + std::to_string(HttpServer::MAX_BODY) + "\r\n\r\n" +
             std::string(HttpServer::MAX_BODY, 'x'));
    TEST_ASSERT_EQUAL(1, p.calls);
    TEST_ASSERT_EQUAL((int)HttpServer::MAX_BODY, (int)p.plain.size());
}

// O corpo para no meio: espera READ_MS e desiste
void test_corpo_incompleto() {
    Probe p;
    setMillis(1000);
    p.b.send("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    TEST_ASSERT_EQUAL(400, p.b.code);
    TEST_ASSERT_TRUE(millis() >= 1000 + HttpServer::READ_MS);
    TEST_ASSERT_EQUAL(0, p.calls);

    // O navegador fecha no meio: desiste na hora
    auto c = p.connect("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    c->open = false;
    setMillis(5000);
    p.srv.handleClient();
    TEST_ASSERT_EQUAL(5000UL, millis());
    TEST_ASSERT_TRUE(c->closed);
    TEST_ASSERT_EQUAL(0, p.calls);
}

// ── conexão sem pedido ───────────────────────────────────────────────────────

void test_conexao_parada_espera_e_desiste() {
    Probe p;
    p.srv.handleClient();                    // ninguém na porta
    setMillis(100);
    auto c = p.connect();
    p.srv.handleClient();
    setMillis(100 + HttpServer::WAIT_MS - 1);
    p.srv.handleClient();
    TEST_ASSERT_FALSE(c->closed);

    // O pedido chega depois, na mesma conexão
    c->rx = "GET /tarde HTTP/1.1\r\n\r\n";
    p.srv.handleClient();
    TEST_ASSERT_EQUAL_STRING("/tarde", p.path.c_str());
    TEST_ASSERT_TRUE(c->closed);

    auto d = p.connect();
    setMillis(1000);
    p.srv.handleClient();
    setMillis(1000 + HttpServer::WAIT_MS);
    p.srv.handleClient();
    TEST_ASSERT_TRUE(d->closed);
    TEST_ASSERT_TRUE(d->tx.empty());
}

void test_conexao_fechada_sem_pedido() {
    Probe p;
    auto c = p.connect();
    c->open = false;
    p.srv.handleClient();                    // descartada no accept
    TEST_ASSERT_TRUE(c->tx.empty());
    TEST_ASSERT_EQUAL(0, p.calls);
    p.b.send("GET /depois HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("/depois", p.path.c_str());

    // Fecha depois de conectar, antes do pedido
    auto d = p.connect();
    p.srv.handleClient();
    d->open = false;
    p.srv.handleClient();
    TEST_ASSERT_TRUE(d->closed);
}

// Pré-conexão do navegador: outra conexão com pedido, ou fila cheia, faz a
// parada ceder depois de YIELD_MS
void test_conexao_parada_cede_a_outra() {
    Probe p;
    setMillis(0);
    auto idle = p.connect();
    p.srv.handleClient();
    auto real = p.connect("GET /real HTTP/1.1\r\n\r\n");
    setMillis(HttpServer::YIELD_MS);
    p.srv.handleClient();
    TEST_ASSERT_FALSE(idle->closed);
    setMillis(HttpServer::YIELD_MS + 1);
    p.srv.handleClient();
    TEST_ASSERT_TRUE(idle->closed);
    p.srv.handleClient();
    TEST_ASSERT_EQUAL_STRING("/real", p.path.c_str());

    auto idle2 = p.connect();
    p.srv.handleClient();
    p.connect();                             // sem dados, mas a fila encheu
    MockServer::maxPending = true;
    setMillis(millis() + HttpServer::YIELD_MS + 1);
    p.srv.handleClient();
    TEST_ASSERT_TRUE(idle2->closed);
}

// ── resposta ─────────────────────────────────────────────────────────────────

// Página em pedaços: sem Content-Length, termina ao fechar; passa do buffer
void test_pagina_em_pedacos() {
    Probe p;
    std::string big(700, 'z');
    p.reply = [&](Probe& q) {
        q.srv.sendHeader("Cache-Control", "no-store");
        q.srv.setContentLength(HttpServer::UNKNOWN_LENGTH);
        q.srv.send(201, "text/html", "");
        q.srv.sendContent_P(PSTR("<p>"));
        q.srv.sendContent(String(big.c_str()));
        q.srv.sendContent("");
        q.srv.sendContent("</p>");
    };
    p.b.send("GET / HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL(201, p.b.code);
    TEST_ASSERT_EQUAL_STRING("no-store", p.b.headers["Cache-Control"].c_str());
    TEST_ASSERT_EQUAL(0, (int)p.b.headers.count("Content-Length"));
    TEST_ASSERT_TRUE(p.b.out == "<p>" + big + "</p>");
    TEST_ASSERT_TRUE(p.b.conn->writes >= 3);   // o buffer esvaziou no meio
    TEST_ASSERT_TRUE(p.b.conn->closed);
}

void test_corpo_da_flash() {
    Probe p;
    static const char DATA[] PROGMEM = "\x1f\x8b binario";
    p.reply = [](Probe& q) {
        q.srv.sendHeader("Content-Encoding", "gzip");
        q.srv.send_P(200, "image/x", DATA, sizeof(DATA) - 1);
    };
    p.b.send("GET / HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("image/x", p.b.contentType.c_str());
    TEST_ASSERT_EQUAL_STRING("gzip", p.b.headers["Content-Encoding"].c_str());
    TEST_ASSERT_EQUAL_STRING(std::to_string(sizeof(DATA) - 1).c_str(),
                             p.b.headers["Content-Length"].c_str());
    TEST_ASSERT_TRUE(p.b.out == std::string(DATA, sizeof(DATA) - 1));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_get_com_query_e_resposta_com_tamanho);
    RUN_TEST(test_post_urlencoded_e_plain);
    RUN_TEST(test_corpo_que_nao_e_formulario);
    RUN_TEST(test_decodificacao_e_nomes_parecidos);
    RUN_TEST(test_cabecalhos);
    RUN_TEST(test_metodo_desconhecido_chega_ao_handler);
    RUN_TEST(test_pedido_malformado);
    RUN_TEST(test_caminho_longo_demais);
    RUN_TEST(test_corpo_grande_demais_ou_negativo);
    RUN_TEST(test_corpo_incompleto);
    RUN_TEST(test_conexao_parada_espera_e_desiste);
    RUN_TEST(test_conexao_fechada_sem_pedido);
    RUN_TEST(test_conexao_parada_cede_a_outra);
    RUN_TEST(test_pagina_em_pedacos);
    RUN_TEST(test_corpo_da_flash);
    return UNITY_END();
}
