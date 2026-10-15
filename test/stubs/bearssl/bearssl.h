// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
// The part of BearSSL the code under test uses: HMAC over SHA-1 (Totp.h),
// with the same names and signatures as the core's bearssl_hmac.h. The
// firmware links the real BearSSL; here a plain SHA-1, checked by the RFC
// 4226/6238 vectors in test_totp.
#include <cstddef>
#include <cstdint>
#include <cstring>

// Trust anchors (bearssl_x509.h): the fields CloudflareCA.h fills
#define BR_KEYTYPE_RSA 1
#define BR_KEYTYPE_EC  2
#define BR_X509_TA_CA  0x0001
struct br_x509_certificate { unsigned char* data; size_t data_len; };
struct br_x500_name { unsigned char* data; size_t len; };
struct br_rsa_public_key { unsigned char* n; size_t nlen; unsigned char* e; size_t elen; };
struct br_ec_public_key { int curve; unsigned char* q; size_t qlen; };
struct br_x509_pkey {
    unsigned char key_type;
    union { br_rsa_public_key rsa; br_ec_public_key ec; } key;
};
struct br_x509_trust_anchor { br_x500_name dn; unsigned flags; br_x509_pkey pkey; };

struct br_hash_class { int id; };
static const br_hash_class br_sha1_vtable = {2};

namespace stub_sha1 {
struct Sha1 {
    uint32_t state[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint32_t bytes = 0;
    uint8_t  buf[64] = {};
    uint8_t  idx = 0;

    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bytes += (uint32_t)len;
        for (size_t i = 0; i < len; i++) {
            buf[idx++] = p[i];
            if (idx == 64) {
                block();
                idx = 0;
            }
        }
    }

    void final(uint8_t out[20]) {
        uint32_t bits = bytes << 3;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (idx != 56) update(&zero, 1);
        const uint8_t lenb[8] = {0, 0, 0, 0, (uint8_t)(bits >> 24), (uint8_t)(bits >> 16),
                                 (uint8_t)(bits >> 8), (uint8_t)bits};
        update(lenb, 8);
        for (int i = 0; i < 5; i++) {
            out[i * 4]     = (uint8_t)(state[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
            out[i * 4 + 3] = (uint8_t)state[i];
        }
    }

    void block() {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[i * 4] << 24) | ((uint32_t)buf[i * 4 + 1] << 16) |
                   ((uint32_t)buf[i * 4 + 2] << 8) | buf[i * 4 + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = t;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }
};
}  // namespace stub_sha1

// A key longer than one block is hashed first (RFC 2104), as BearSSL does
struct br_hmac_key_context { uint8_t k[64]; };
struct br_hmac_context { stub_sha1::Sha1 inner; uint8_t k[64]; };

inline void br_hmac_key_init(br_hmac_key_context* kc, const br_hash_class*, const void* key,
                             size_t key_len) {
    memset(kc->k, 0, sizeof(kc->k));
    if (key_len > sizeof(kc->k)) {
        stub_sha1::Sha1 h;
        h.update(key, key_len);
        h.final(kc->k);
        return;
    }
    memcpy(kc->k, key, key_len);
}

inline void br_hmac_init(br_hmac_context* ctx, const br_hmac_key_context* kc, size_t) {
    uint8_t ipad[64];
    for (int i = 0; i < 64; i++) ipad[i] = kc->k[i] ^ 0x36;
    ctx->inner = stub_sha1::Sha1();
    ctx->inner.update(ipad, 64);
    memcpy(ctx->k, kc->k, 64);
}

inline void br_hmac_update(br_hmac_context* ctx, const void* data, size_t len) {
    ctx->inner.update(data, len);
}

inline size_t br_hmac_out(const br_hmac_context* ctx, void* out) {
    uint8_t in[20], opad[64];
    stub_sha1::Sha1 h = ctx->inner;
    h.final(in);
    for (int i = 0; i < 64; i++) opad[i] = ctx->k[i] ^ 0x5c;
    stub_sha1::Sha1 o;
    o.update(opad, 64);
    o.update(in, 20);
    o.final(static_cast<uint8_t*>(out));
    return 20;
}
