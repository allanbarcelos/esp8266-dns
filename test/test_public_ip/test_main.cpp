#include <unity.h>
#include "../test_support.h"
#include "PublicIPResolver.h"

static Logger logger;

void setUp()    { resetMocks(); logger = Logger(); }
void tearDown() {}

void test_primeiro_servico_ok() {
    PublicIPResolver r(logger);
    MockHTTP::push(200, "1.2.3.4");
    char out[16] = "";
    TEST_ASSERT_TRUE(r.resolve(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", out);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("http://api.ipify.org", MockHTTP::calledURLs[0].c_str());
}

void test_aceita_espacos_e_quebra_de_linha() {
    PublicIPResolver r(logger);
    MockHTTP::push(200, "  5.6.7.8\n");
    char out[16] = "";
    TEST_ASSERT_TRUE(r.resolve(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("5.6.7.8", out);
}

void test_falha_de_conexao_tenta_proximo() {
    PublicIPResolver r(logger);
    MockHTTP::failNextBegin();
    MockHTTP::push(200, "9.9.9.9");
    char out[16] = "";
    TEST_ASSERT_TRUE(r.resolve(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("9.9.9.9", out);
    TEST_ASSERT_EQUAL(2, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "falha ao conectar"));
}

void test_http_erro_tenta_proximo() {
    PublicIPResolver r(logger);
    MockHTTP::push(503);
    MockHTTP::push(200, "8.8.4.4");
    char out[16] = "";
    TEST_ASSERT_TRUE(r.resolve(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("8.8.4.4", out);
    TEST_ASSERT_TRUE(logHas(logger, "HTTP 503"));
}

void test_resposta_invalida_tenta_proximo() {
    PublicIPResolver r(logger);
    MockHTTP::push(200, "<html>erro</html>");
    MockHTTP::push(200, "300.1.1.1");
    MockHTTP::push(200, "10.0.0.1");
    char out[16] = "";
    TEST_ASSERT_TRUE(r.resolve(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", out);
    TEST_ASSERT_TRUE(logHas(logger, "resposta invalida"));
}

void test_todos_falham_nao_altera_saida() {
    PublicIPResolver r(logger);
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(500);
    char out[16] = "anterior";
    TEST_ASSERT_FALSE(r.resolve(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("anterior", out);
    TEST_ASSERT_EQUAL(NUM_IP_SERVICES, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "Todos os servicos"));
}

void test_buffer_pequeno_rejeita() {
    PublicIPResolver r(logger);
    for (int i = 0; i < NUM_IP_SERVICES; i++) MockHTTP::push(200, "100.100.100.100");
    char out[8] = "x";
    TEST_ASSERT_FALSE(r.resolve(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("x", out);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_primeiro_servico_ok);
    RUN_TEST(test_aceita_espacos_e_quebra_de_linha);
    RUN_TEST(test_falha_de_conexao_tenta_proximo);
    RUN_TEST(test_http_erro_tenta_proximo);
    RUN_TEST(test_resposta_invalida_tenta_proximo);
    RUN_TEST(test_todos_falham_nao_altera_saida);
    RUN_TEST(test_buffer_pequeno_rejeita);
    return UNITY_END();
}
