// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include <bearssl/bearssl.h>

// Authenticator codes (RFC 6238): HMAC-SHA1, 30 s step, 6 digits, ±1 step.
// HMAC and SHA-1 come from BearSSL: the TLS stack keeps both (certificate
// chains and the TLS 1.2 PRF), so they cost nothing more here.
// An all-zero key means "not enrolled"; a real key never is.
struct Totp {
    static constexpr int KEY_LEN = 20;
    static constexpr uint32_t STEP = 30;

    static bool blank(const uint8_t* k) {
        uint8_t d = 0;
        for (int i = 0; i < KEY_LEN; i++) d |= k[i];
        return d == 0;
    }

    // 159 bits from the hardware RNG. The low bit is forced on so the key
    // cannot collide with "not enrolled".
    static void randomKey(uint8_t* k) {
        for (int i = 0; i < KEY_LEN; i += 4) {
            uint32_t r = ESP.random();
            k[i]     = (uint8_t)r;
            k[i + 1] = (uint8_t)(r >> 8);
            k[i + 2] = (uint8_t)(r >> 16);
            k[i + 3] = (uint8_t)(r >> 24);
        }
        k[0] |= 1;
    }

    static void toHex(const uint8_t* k, char* out) {
        static const char H[] = "0123456789abcdef";
        for (int i = 0; i < KEY_LEN; i++) {
            out[i * 2]     = H[k[i] >> 4];
            out[i * 2 + 1] = H[k[i] & 0xf];
        }
        out[KEY_LEN * 2] = '\0';
    }

    // 40 hex digits. Anything else, including all zeros, leaves k cleared.
    static bool fromHex(const char* s, uint8_t* k) {
        memset(k, 0, KEY_LEN);
        if (!s || strlen(s) != (size_t)KEY_LEN * 2) return false;
        for (int i = 0; i < KEY_LEN; i++) {
            int hi = nibble(s[i * 2]), lo = nibble(s[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                memset(k, 0, KEY_LEN);
                return false;
            }
            k[i] = (uint8_t)((hi << 4) | lo);
        }
        if (!blank(k)) return true;
        memset(k, 0, KEY_LEN);
        return false;
    }

    // 20 bytes -> 32 characters, no padding. What authenticator apps ask for.
    static void toBase32(const uint8_t* k, char* out) {
        static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        uint32_t buf = 0;   // only the low bits not yet written matter
        int bits = 0, n = 0;
        for (int i = 0; i < KEY_LEN; i++) {
            buf = ((buf << 8) | k[i]) & 0xFFFFu;
            bits += 8;
            while (bits >= 5) {
                bits -= 5;
                out[n++] = A[(buf >> bits) & 31];
            }
        }
        out[n] = '\0';
    }

    // 32 characters. Spaces are ignored and '=' stops the decode. A short
    // string, a bad character or leftover bits clears k.
    static bool fromBase32(const char* s, uint8_t* k) {
        memset(k, 0, KEY_LEN);
        if (!s) return false;
        uint32_t buf = 0;
        int bits = 0, n = 0;
        for (int i = 0; s[i] && s[i] != '='; i++) {
            if (s[i] == ' ') continue;
            int v = b32(s[i]);
            if (v < 0) {
                memset(k, 0, KEY_LEN);
                return false;
            }
            buf = ((buf << 5) | (uint32_t)v) & 0xFFFFu;
            bits += 5;
            if (bits >= 8) {
                bits -= 8;
                if (n == KEY_LEN) {
                    memset(k, 0, KEY_LEN);
                    return false;
                }
                k[n++] = (uint8_t)((buf >> bits) & 0xff);
            }
        }
        if (n != KEY_LEN || bits != 0) {
            memset(k, 0, KEY_LEN);
            return false;
        }
        return true;
    }

    static uint32_t hotp(const uint8_t* key, uint32_t counter) {
        uint8_t msg[8] = {
            0, 0, 0, 0,
            (uint8_t)(counter >> 24), (uint8_t)(counter >> 16),
            (uint8_t)(counter >> 8),  (uint8_t)counter
        };
        uint8_t mac[20];
        hmac(key, msg, mac);
        int off = mac[19] & 0x0f;
        uint32_t bin = ((uint32_t)(mac[off] & 0x7f) << 24) | ((uint32_t)mac[off + 1] << 16) |
                       ((uint32_t)mac[off + 2] << 8) | mac[off + 3];
        return bin % 1000000u;
    }

    static void codeAt(const uint8_t* key, time_t now, char out[7]) {
        format(hotp(key, (uint32_t)now / STEP), out);
    }

    // Accepts the current step and one step either side. now == 0 is refused
    // (the clock is still in 1970). The code is exactly 6 digits.
    // With last, a step at or before *last is refused (RFC 6238 §5.2: a code
    // works once, and a clock pushed back does not bring old codes back), and
    // the step that matched is written there.
    static bool verify(const uint8_t* key, time_t now, const char* code, uint32_t* last = nullptr) {
        if (now <= 0 || !code) return false;
        for (int i = 0; i < 6; i++)
            if (code[i] < '0' || code[i] > '9') return false;
        if (code[6] != '\0') return false;
        uint32_t step = (uint32_t)now / STEP;
        uint32_t hit = 0;
        uint8_t ok = 0;
        for (int w = -1; w <= 1; w++) {
            if (w < 0 && step == 0) continue;
            uint32_t c = w < 0 ? step - 1 : step + (uint32_t)w;
            char expect[7];
            format(hotp(key, c), expect);
            uint8_t d = 0;
            for (int i = 0; i < 6; i++) d |= (uint8_t)(expect[i] ^ code[i]);
            uint8_t m = (uint8_t)(d == 0 && (!last || c > *last));
            ok |= m;
            if (m) hit = c;
        }
        if (ok && last) *last = hit;
        return ok != 0;
    }

private:
    static int nibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static int b32(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a';
        if (c >= '2' && c <= '7') return c - '2' + 26;
        return -1;
    }

    static void format(uint32_t otp, char out[7]) {
        for (int i = 5; i >= 0; i--) {
            out[i] = (char)('0' + otp % 10);
            otp /= 10;
        }
        out[6] = '\0';
    }

    static void hmac(const uint8_t* key, const uint8_t msg[8], uint8_t out[20]) {
        br_hmac_key_context kc;
        br_hmac_context hc;
        br_hmac_key_init(&kc, &br_sha1_vtable, key, KEY_LEN);
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, msg, 8);
        br_hmac_out(&hc, out);
    }
};
