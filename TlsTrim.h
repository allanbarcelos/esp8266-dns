// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
// Tira do firmware a parte do BearSSL que o aparelho nunca usa (~27 KB).
//
// O WiFiClientSecure do core instala todas as cifras e curvas, e o linker
// leva tudo junto. Aqui o sketch define as mesmas funções, vazias; o linker
// fica com estas e não puxa os objetos da biblioteca. Só pode entrar no
// .ino (um único arquivo compilado no firmware; os testes nativos não usam).
//
// Funciona porque todo TlsClient anuncia só ECDHE + AES-GCM
// (TlsClient::SUITES): o servidor nunca escolhe uma cifra removida.
//   - CBC, CCM, 3DES e ChaCha20: as cifras saem; AES-GCM fica.
//   - X25519: a troca de chaves usa P-256; P-384/P-521 ficam para validar
//     certificados (cadeias ECDSA como a da Let's Encrypt usam P-384).
//   - Certificado de cliente (assinatura RSA/ECDSA): o aparelho não usa.
#include <bearssl/bearssl.h>

extern "C" {

void br_ssl_engine_set_default_aes_cbc(br_ssl_engine_context*) {}
void br_ssl_engine_set_default_aes_ccm(br_ssl_engine_context*) {}
void br_ssl_engine_set_default_des_cbc(br_ssl_engine_context*) {}
void br_ssl_engine_set_default_chapol(br_ssl_engine_context*) {}

void br_ssl_client_set_single_rsa(br_ssl_client_context*, const br_x509_certificate*, size_t,
                                  const br_rsa_private_key*, br_rsa_pkcs1_sign) {}
void br_ssl_client_set_single_ec(br_ssl_client_context*, const br_x509_certificate*, size_t,
                                 const br_ec_private_key*, unsigned, unsigned,
                                 const br_ec_impl*, br_ecdsa_sign) {}
br_rsa_pkcs1_sign br_rsa_pkcs1_sign_get_default() { return nullptr; }
br_ecdsa_sign br_ecdsa_sign_asn1_get_default() { return nullptr; }

// br_ec_all_m15 do BearSSL sem Curve25519: P-256 na implementação rápida,
// P-384 e P-521 na genérica. É a que br_ec_get_default() devolve.
#define TLS_EC(call) (curve == BR_EC_secp256r1 ? br_ec_p256_m15.call : br_ec_prime_i15.call)

static const unsigned char* _ecGenerator(int curve, size_t* len) {
    return TLS_EC(generator(curve, len));
}
static const unsigned char* _ecOrder(int curve, size_t* len) {
    return TLS_EC(order(curve, len));
}
static size_t _ecXoff(int curve, size_t* len) {
    return TLS_EC(xoff(curve, len));
}
static uint32_t _ecMul(unsigned char* G, size_t Glen, const unsigned char* k, size_t klen,
                       int curve) {
    return TLS_EC(mul(G, Glen, k, klen, curve));
}
static size_t _ecMulgen(unsigned char* R, const unsigned char* x, size_t xlen, int curve) {
    return TLS_EC(mulgen(R, x, xlen, curve));
}
static uint32_t _ecMuladd(unsigned char* A, const unsigned char* B, size_t len,
                          const unsigned char* x, size_t xlen, const unsigned char* y,
                          size_t ylen, int curve) {
    return TLS_EC(muladd(A, B, len, x, xlen, y, ylen, curve));
}
#undef TLS_EC

extern const br_ec_impl br_ec_all_m15;
const br_ec_impl br_ec_all_m15 = {
    (uint32_t)((1u << BR_EC_secp256r1) | (1u << BR_EC_secp384r1) | (1u << BR_EC_secp521r1)),
    &_ecGenerator, &_ecOrder, &_ecXoff, &_ecMul, &_ecMulgen, &_ecMuladd,
};

}  // extern "C"
