// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
// Removes from the firmware the part of BearSSL the device never uses (~29 KB),
// and two pieces of the C runtime it never needs (~10 KB, at the end).
//
// The core's WiFiClientSecure installs every cipher and curve, and the linker
// pulls all of it in. Here the sketch defines the same functions, empty; the
// linker keeps these and does not pull the library objects. It may only be
// included from the .ino (a single file compiled into the firmware; the
// native tests do not use it).
//
// It works because every TlsClient announces only ECDHE + AES-GCM
// (TlsClient::SUITES): the server never picks a removed cipher.
//   - CBC, CCM, 3DES and ChaCha20: those ciphers go; AES-GCM stays.
//   - X25519: key exchange uses P-256; P-384/P-521 stay to validate
//     certificates (ECDSA chains such as Let's Encrypt's use P-384).
//   - MD5 and the TLS 1.0/1.1 PRF: AES-GCM only exists in TLS 1.2, and
//     BearSSL rejects a TLS 1.2 suite on an older version as soon as the
//     ServerHello arrives. In TLS 1.2 the handshake and Finished use SHA-256
//     or SHA-384, and the client never offers MD5 for signatures. An MD5
//     certificate gets a hash of size 0: it fails, closed.
//   - Client certificate (RSA/ECDSA signing): the device does not use it.
// Everything that stays runs as before: P-256 keeps the fast implementation,
// and SHA-224 and SHA-512 share their objects with SHA-256 and SHA-384.
#include <Arduino.h>
#include <bearssl/bearssl.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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

// Registered by WiFiClientSecure as the MD5 implementation. Its output size
// is 0, so any use of it fails.
static void _md5Init(const br_hash_class**) {}
static void _md5Update(const br_hash_class**, const void*, size_t) {}
static void _md5Out(const br_hash_class* const*, void*) {}
static uint64_t _md5State(const br_hash_class* const*, void*) { return 0; }
static void _md5Set(const br_hash_class**, const void*, uint64_t) {}
const br_hash_class br_md5_vtable = {
    sizeof(br_hash_class*), 0, _md5Init, _md5Update, _md5Out, _md5State, _md5Set,
};

// TLS 1.0/1.1 only (see above); a call would fill zeros and the Finished
// check would fail.
void br_tls10_prf(void* dst, size_t len, const void*, size_t, const char*, size_t,
                  const br_tls_prf_seed_chunk*) {
    if (dst && len) memset(dst, 0, len);
}

// BearSSL's br_ec_all_m15 without Curve25519: P-256 on the fast
// implementation, P-384 and P-521 on the generic one. It is what
// br_ec_get_default() returns.
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

// ── C runtime ────────────────────────────────────────────────────────────────

// The core registers the exception frames at boot, which links the libgcc
// unwinder (~4.2 KB). The firmware is built with -fno-exceptions: nothing
// ever unwinds, and the crash dump does not use it.
void __register_frame_info(const void*, void*) {}

// printf/vprintf (lwIP and SDK diagnostics) pull newlib's whole FILE layer
// (stdout buffering, fflush, fvwrite: ~6 KB) that nothing else uses. They go
// through vsnprintf, already in the firmware, and the ROM's serial output.
// The only difference: a line is cut at 127 characters.
int vprintf(const char* fmt, va_list ap) {
    char buf[128];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    ets_printf("%s", buf);
    return n;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

}  // extern "C"
