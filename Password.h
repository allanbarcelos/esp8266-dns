// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include <bearssl/bearssl.h>

// The panel password is stored as PBKDF2-HMAC-SHA1 (RFC 8018) with a random
// salt, never as typed: someone who reads the flash (it is not encrypted)
// does not learn a password that may open other things. HMAC-SHA1 comes
// from BearSSL, already in the firmware for TLS and the authenticator.
//
// Stored text: "pbkdf2:" + 32 hex digits of salt + ":" + 40 hex digits of
// the derived key. Anything else is a password from before the hash (plain
// text), still accepted so the update does not lock anyone out;
// ConfigStore replaces it with the hash on the first boot.
struct Password {
    static constexpr size_t   SALT_LEN = 16;
    static constexpr size_t   KEY_LEN  = 20;           // one SHA-1 block
    static constexpr uint32_t ROUNDS   = 4096;         // ~0.2 s on the ESP-01
    static constexpr size_t   MAX_LEN  = 64;           // the panel's limit
    static constexpr size_t   TEXT_LEN = 7 + SALT_LEN * 2 + 1 + KEY_LEN * 2;

    // PBKDF2-HMAC-SHA1, first block (RFC 6070 has vectors)
    static void derive(const char* pass, size_t n, const uint8_t* salt, size_t saltLen,
                       uint32_t rounds, uint8_t out[KEY_LEN]) {
        br_hmac_key_context kc;
        br_hmac_context hc;
        br_hmac_key_init(&kc, &br_sha1_vtable, pass, n);
        static const uint8_t ONE[4] = {0, 0, 0, 1};
        uint8_t u[KEY_LEN];
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, salt, saltLen);
        br_hmac_update(&hc, ONE, sizeof(ONE));
        br_hmac_out(&hc, u);
        memcpy(out, u, KEY_LEN);
        for (uint32_t r = 1; r < rounds; r++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, KEY_LEN);
            br_hmac_out(&hc, u);
            for (size_t i = 0; i < KEY_LEN; i++) out[i] ^= u[i];
        }
    }

    // The text to store for pass, with a new salt from the hardware RNG
    static String hash(const String& pass) {
        uint8_t salt[SALT_LEN];
        for (size_t i = 0; i < SALT_LEN; i += 4) {
            uint32_t r = ESP.random();
            memcpy(salt + i, &r, 4);
        }
        return _text(pass, salt);
    }

    static bool hashed(const String& stored) {
        return stored.length() == TEXT_LEN && strncmp(stored.c_str(), "pbkdf2:", 7) == 0;
    }

    // In time that does not depend on how much of the attempt is right
    static bool check(const String& attempt, const String& stored) {
        if (attempt.length() > MAX_LEN || stored.length() == 0) return false;
        if (!hashed(stored)) return _same(attempt.c_str(), stored.c_str(), attempt.length(), stored.length());
        uint8_t salt[SALT_LEN];
        _unhex(stored.c_str() + 7, salt, SALT_LEN);
        String expect = _text(attempt, salt);
        return _same(expect.c_str(), stored.c_str(), TEXT_LEN, TEXT_LEN);
    }

private:
    static String _text(const String& pass, const uint8_t salt[SALT_LEN]) {
        uint8_t key[KEY_LEN];
        derive(pass.c_str(), pass.length(), salt, SALT_LEN, ROUNDS, key);
        char out[TEXT_LEN + 1] = "pbkdf2:";
        _hex(salt, SALT_LEN, out + 7);
        out[7 + SALT_LEN * 2] = ':';
        _hex(key, KEY_LEN, out + 8 + SALT_LEN * 2);
        return String(out);
    }

    static void _hex(const uint8_t* b, size_t n, char* out) {
        static const char H[] = "0123456789abcdef";
        for (size_t i = 0; i < n; i++) {
            out[i * 2] = H[b[i] >> 4];
            out[i * 2 + 1] = H[b[i] & 15];
        }
        out[n * 2] = '\0';
    }

    // Lowercase hex, as _hex writes it (hashed() already checked the length)
    static void _unhex(const char* s, uint8_t* out, size_t n) {
        for (size_t i = 0; i < n; i++) {
            uint8_t v = 0;
            for (int k = 0; k < 2; k++) {
                char c = s[i * 2 + k];
                v = (uint8_t)(v << 4 | (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10));
            }
            out[i] = v;
        }
    }

    // Fixed-length compare over the longest password the panel stores
    static bool _same(const char* a, const char* b, size_t na, size_t nb) {
        uint8_t d = na != nb;
        size_t cap = na > MAX_LEN ? na : MAX_LEN;
        for (size_t i = 0; i < cap; i++)
            d |= (uint8_t)((i < na ? a[i] : 0) ^ (i < nb ? b[i] : 0));
        return d == 0;
    }
};
