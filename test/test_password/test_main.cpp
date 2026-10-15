// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Password.h"

void setUp()    { resetMocks(); }
void tearDown() {}

static std::string derived(const char* pass, const char* salt, uint32_t rounds) {
    uint8_t out[Password::KEY_LEN];
    Password::derive(pass, std::strlen(pass), reinterpret_cast<const uint8_t*>(salt), std::strlen(salt),
                     rounds, out);
    std::string s;
    char b[3];
    for (uint8_t c : out) {
        std::snprintf(b, sizeof(b), "%02x", c);
        s += b;
    }
    return s;
}

// RFC 6070, PBKDF2-HMAC-SHA1, the first 20 bytes
void test_rfc6070_vectors() {
    TEST_ASSERT_EQUAL_STRING("0c60c80f961f0e71f3a9b524af6012062fe037a6", derived("password", "salt", 1).c_str());
    TEST_ASSERT_EQUAL_STRING("ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957", derived("password", "salt", 2).c_str());
    TEST_ASSERT_EQUAL_STRING("4b007901b765489abead49d926f721d065a429c1", derived("password", "salt", 4096).c_str());
    TEST_ASSERT_EQUAL_STRING("3d2eec4fe41c849b80c8d83662c0e44a8b291a96",
        derived("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096).c_str());
}

void test_hash_format_and_salt() {
    String a = Password::hash("secret123");
    String b = Password::hash("secret123");
    TEST_ASSERT_EQUAL((int)Password::TEXT_LEN, (int)a.length());
    TEST_ASSERT_EQUAL(0, std::strncmp(a.c_str(), "pbkdf2:", 7));
    TEST_ASSERT_EQUAL(':', a.c_str()[7 + 32]);
    TEST_ASSERT_TRUE(Password::hashed(a));
    TEST_ASSERT_FALSE(a == b);                                  // a new salt each time
    TEST_ASSERT_TRUE(std::strstr(a.c_str(), "secret") == nullptr);
}

void test_check() {
    String stored = Password::hash("secret123");
    TEST_ASSERT_TRUE(Password::check("secret123", stored));
    TEST_ASSERT_FALSE(Password::check("secret124", stored));
    TEST_ASSERT_FALSE(Password::check("", stored));
    TEST_ASSERT_FALSE(Password::check(String(std::string(65, 'x').c_str()), stored));   // longer than any
    TEST_ASSERT_FALSE(Password::check("secret123", ""));        // no password set

    // A password from before the hash, as plain text
    TEST_ASSERT_FALSE(Password::hashed("strong-pass"));
    TEST_ASSERT_TRUE(Password::check("strong-pass", "strong-pass"));
    TEST_ASSERT_FALSE(Password::check("strong-pas", "strong-pass"));
    TEST_ASSERT_FALSE(Password::check("strong-pass!", "strong-pass"));

    // Text of the right size that does not start with the marker is plain text
    String odd = stored;
    odd.begin()[0] = 'P';
    TEST_ASSERT_FALSE(Password::hashed(odd));
    // Salt digits in uppercase still parse, but the text no longer matches
    String upper = stored;
    for (size_t i = 7; i < 7 + 32; i++) upper.begin()[i] = (char)toupper(upper.c_str()[i]);
    TEST_ASSERT_FALSE(upper == stored);
    TEST_ASSERT_FALSE(Password::check("secret123", upper));      // the text no longer matches
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_rfc6070_vectors);
    RUN_TEST(test_hash_format_and_salt);
    RUN_TEST(test_check);
    return UNITY_END();
}
