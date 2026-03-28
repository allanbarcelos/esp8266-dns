#include <unity.h>
#include "Logger.h"

static Logger lgr;

void setUp()    { lgr = Logger(); mock::_millis = 0; }
void tearDown() {}

// ── Testes ───────────────────────────────────────────────────────────────────

void test_single_entry() {
    lgr.log("hello %s", "world");

    TEST_ASSERT_EQUAL(1, lgr.count());
    TEST_ASSERT_NOT_NULL(std::strstr(lgr.entry(0), "hello world"));
}

void test_timestamp_in_entry() {
    setMillis(5000);
    lgr.log("msg");

    // Formato: "[5s] msg"
    TEST_ASSERT_NOT_NULL(std::strstr(lgr.entry(0), "[5s]"));
}

void test_buffer_nao_excede_limite() {
    for (int i = 0; i < LOG_BUFFER_SIZE + 3; i++)
        lgr.log("msg %d", i);

    TEST_ASSERT_EQUAL(LOG_BUFFER_SIZE, lgr.count());
}

void test_buffer_circular_ordem_correta() {
    // Escreve LOG_BUFFER_SIZE + 2 entradas; as primeiras 2 devem ter sido sobrescritas
    for (int i = 0; i < LOG_BUFFER_SIZE + 2; i++)
        lgr.log("item %d", i);

    // A entrada mais antiga disponível deve ser "item 2"
    int start = lgr.startIndex();
    TEST_ASSERT_NOT_NULL(std::strstr(lgr.entry(start), "item 2"));

    // A última deve ser "item 11" (LOG_BUFFER_SIZE + 2 - 1 = 11)
    int last = (start + lgr.count() - 1) % LOG_BUFFER_SIZE;
    char expected[32];
    std::snprintf(expected, sizeof(expected), "item %d", LOG_BUFFER_SIZE + 1);
    TEST_ASSERT_NOT_NULL(std::strstr(lgr.entry(last), expected));
}

void test_count_antes_do_wrap() {
    lgr.log("a");
    lgr.log("b");
    lgr.log("c");

    TEST_ASSERT_EQUAL(3, lgr.count());
    TEST_ASSERT_EQUAL(0, lgr.startIndex()); // sem wrap ainda
}

void test_count_apos_wrap() {
    for (int i = 0; i < LOG_BUFFER_SIZE; i++)
        lgr.log("x");

    // Exatamente no limite: não wrappou ainda
    TEST_ASSERT_EQUAL(LOG_BUFFER_SIZE, lgr.count());

    lgr.log("extra"); // agora wrappou
    TEST_ASSERT_EQUAL(LOG_BUFFER_SIZE, lgr.count());
    TEST_ASSERT_EQUAL(1, lgr.startIndex()); // head avançou para 1
}

// ── Runner ────────────────────────────────────────────────────────────────────
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_single_entry);
    RUN_TEST(test_timestamp_in_entry);
    RUN_TEST(test_buffer_nao_excede_limite);
    RUN_TEST(test_buffer_circular_ordem_correta);
    RUN_TEST(test_count_antes_do_wrap);
    RUN_TEST(test_count_apos_wrap);
    return UNITY_END();
}
