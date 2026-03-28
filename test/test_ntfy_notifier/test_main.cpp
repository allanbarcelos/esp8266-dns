#include <unity.h>
#include "Config.h"
#include "Logger.h"
#include "NtfyNotifier.h"
#include <cstring>

void setUp()    { MockHTTP::reset(); mock::_millis = 0; }
void tearDown() {}

// ── Testes ───────────────────────────────────────────────────────────────────

void test_sem_envio_sem_ip() {
    Logger lgr; Config cfg;
    std::memset(cfg.publicIP, 0, sizeof(cfg.publicIP));
    NtfyNotifier ntfy(cfg, lgr);

    ntfy.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_envia_no_boot_ao_ter_ip() {
    Logger lgr; Config cfg;
    std::strncpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    NtfyNotifier ntfy(cfg, lgr);

    ntfy.tick(0);

    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_NOT_NULL(std::strstr(MockHTTP::calledURLs[0].c_str(), "ntfy.sh"));
}

void test_nao_reenvia_antes_do_intervalo() {
    Logger lgr; Config cfg;
    std::strncpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    NtfyNotifier ntfy(cfg, lgr);

    ntfy.tick(0);                                    // envio de boot
    MockHTTP::calledURLs.clear();

    ntfy.tick(NtfyNotifier::SEND_INTERVAL - 1);      // antes do intervalo
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_reenvia_apos_intervalo() {
    Logger lgr; Config cfg;
    std::strncpy(cfg.publicIP, "5.6.7.8", sizeof(cfg.publicIP));
    NtfyNotifier ntfy(cfg, lgr);

    ntfy.tick(0);                                    // boot
    MockHTTP::calledURLs.clear();

    ntfy.tick(NtfyNotifier::SEND_INTERVAL + 1);      // após 1 hora
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_apenas_um_envio_de_boot() {
    Logger lgr; Config cfg;
    std::strncpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    NtfyNotifier ntfy(cfg, lgr);

    ntfy.tick(0);
    ntfy.tick(1);
    ntfy.tick(2);

    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

// ── Runner ────────────────────────────────────────────────────────────────────
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sem_envio_sem_ip);
    RUN_TEST(test_envia_no_boot_ao_ter_ip);
    RUN_TEST(test_nao_reenvia_antes_do_intervalo);
    RUN_TEST(test_reenvia_apos_intervalo);
    RUN_TEST(test_apenas_um_envio_de_boot);
    return UNITY_END();
}
