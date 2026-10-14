// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <cstring>
#include <string>
#include <unity.h>
#include "../test_support.h"
#include "Totp.h"

// RFC 4226 / RFC 6238. ASCII "12345678901234567890", base32 without padding.
static const uint8_t RFC_KEY[Totp::KEY_LEN] = {
    '1','2','3','4','5','6','7','8','9','0','1','2','3','4','5','6','7','8','9','0'
};
static const char* RFC_B32 = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
static const char* RFC_HEX = "3132333435363738393031323334353637383930";

void setUp()    { resetMocks(); }
void tearDown() {}

static void fill(uint8_t* k, uint8_t v) { std::memset(k, v, Totp::KEY_LEN); }

void test_rfc_hotp_and_totp() {
    char code[8];
    TEST_ASSERT_EQUAL_UINT32(755224u, Totp::hotp(RFC_KEY, 0));
    TEST_ASSERT_EQUAL_UINT32(287082u, Totp::hotp(RFC_KEY, 1));
    TEST_ASSERT_EQUAL_UINT32(359152u, Totp::hotp(RFC_KEY, 2));
    Totp::codeAt(RFC_KEY, 59, code);
    TEST_ASSERT_EQUAL_STRING("287082", code);
    Totp::codeAt(RFC_KEY, 0, code);
    TEST_ASSERT_EQUAL_STRING("755224", code);
}

void test_hex_codec() {
    uint8_t k[Totp::KEY_LEN];
    fill(k, 0xFF);
    TEST_ASSERT_FALSE(Totp::fromHex(nullptr, k));
    TEST_ASSERT_TRUE(Totp::blank(k));

    fill(k, 0xFF);
    TEST_ASSERT_FALSE(Totp::fromHex("abc", k));
    TEST_ASSERT_TRUE(Totp::blank(k));
    TEST_ASSERT_FALSE(Totp::fromHex("313233343536373839303132333435363738393", k));   // 39
    TEST_ASSERT_FALSE(Totp::fromHex("31323334353637383930313233343536373839301", k)); // 41

    // High nibble, then low nibble
    TEST_ASSERT_FALSE(Totp::fromHex("g123333435363738393031323334353637383930", k));
    TEST_ASSERT_TRUE(Totp::blank(k));
    TEST_ASSERT_FALSE(Totp::fromHex("0g33333435363738393031323334353637383930", k));
    TEST_ASSERT_TRUE(Totp::blank(k));
    TEST_ASSERT_FALSE(Totp::fromHex("/123333435363738393031323334353637383930", k));
    TEST_ASSERT_TRUE(Totp::blank(k));

    TEST_ASSERT_FALSE(Totp::fromHex("0000000000000000000000000000000000000000", k));
    TEST_ASSERT_TRUE(Totp::blank(k));

    TEST_ASSERT_TRUE(Totp::fromHex(RFC_HEX, k));
    TEST_ASSERT_EQUAL_MEMORY(RFC_KEY, k, Totp::KEY_LEN);

    uint8_t mixed[Totp::KEY_LEN];
    TEST_ASSERT_TRUE(Totp::fromHex("aB00000000000000000000000000000000000001", mixed));
    TEST_ASSERT_EQUAL_UINT8(0xAB, mixed[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, mixed[19]);
    char out[41];
    Totp::toHex(mixed, out);
    TEST_ASSERT_EQUAL_STRING("ab00000000000000000000000000000000000001", out);
    uint8_t back[Totp::KEY_LEN];
    TEST_ASSERT_TRUE(Totp::fromHex(out, back));
    TEST_ASSERT_EQUAL_MEMORY(mixed, back, Totp::KEY_LEN);
}

void test_base32_codec() {
    uint8_t k[Totp::KEY_LEN];
    char out[33];
    Totp::toBase32(RFC_KEY, out);
    TEST_ASSERT_EQUAL_STRING(RFC_B32, out);

    fill(k, 0xFF);
    TEST_ASSERT_FALSE(Totp::fromBase32(nullptr, k));
    TEST_ASSERT_TRUE(Totp::blank(k));
    TEST_ASSERT_FALSE(Totp::fromBase32("", k));
    TEST_ASSERT_FALSE(Totp::fromBase32("   ", k));
    TEST_ASSERT_FALSE(Totp::fromBase32("GEZD", k));
    TEST_ASSERT_TRUE(Totp::blank(k));
    TEST_ASSERT_FALSE(Totp::fromBase32("GEZD====", k));
    TEST_ASSERT_TRUE(Totp::blank(k));
    TEST_ASSERT_FALSE(Totp::fromBase32("GEZDGNBV0Y3TQOJQGEZDGNBVGY3TQOJQ", k)); // '0' is not base32
    TEST_ASSERT_TRUE(Totp::blank(k));
    TEST_ASSERT_FALSE(Totp::fromBase32("{", k));
    TEST_ASSERT_FALSE(Totp::fromBase32("8", k));
    TEST_ASSERT_TRUE(Totp::blank(k));

    TEST_ASSERT_TRUE(Totp::fromBase32("GEZD GNBV GY3T QOJQ GEZD GNBV GY3T QOJQ", k));
    TEST_ASSERT_EQUAL_MEMORY(RFC_KEY, k, Totp::KEY_LEN);
    TEST_ASSERT_TRUE(Totp::fromBase32("gezdgnbvgy3tqojqgezdgnbvgy3tqojq", k));
    TEST_ASSERT_EQUAL_MEMORY(RFC_KEY, k, Totp::KEY_LEN);
    std::string padded = std::string(RFC_B32) + "=";
    TEST_ASSERT_TRUE(Totp::fromBase32(padded.c_str(), k));
    std::string spaced = std::string(RFC_B32) + " ";
    TEST_ASSERT_TRUE(Totp::fromBase32(spaced.c_str(), k));

    // 33rd character leaves leftover bits; 34th would emit a 21st byte
    std::string extra = std::string(RFC_B32) + "A";
    fill(k, 0xFF);
    TEST_ASSERT_FALSE(Totp::fromBase32(extra.c_str(), k));
    TEST_ASSERT_TRUE(Totp::blank(k));
    extra += "A";
    fill(k, 0xFF);
    TEST_ASSERT_FALSE(Totp::fromBase32(extra.c_str(), k));
    TEST_ASSERT_TRUE(Totp::blank(k));
}

void test_verify_accepts_one_step_either_side() {
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 0, "755224"));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, -1, "755224"));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, nullptr));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "75522"));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "75522/"));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "75522a"));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "2870821"));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "000000"));

    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 59, "287082"));  // current step
    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 59, "755224"));  // one step behind
    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 59, "359152"));  // one step ahead

    // Step 0 has no previous counter
    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 29, "755224"));
    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 29, "287082"));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 29, "359152"));

    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 30, "755224"));
}

// With last: a step at or before it is refused, and the step that matched
// is written back (RFC 6238 §5.2)
void test_verify_refuses_spent_steps() {
    uint32_t last = 0;   // step 0 (1970) counts as spent
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "755224", &last));
    TEST_ASSERT_EQUAL_UINT32(0u, last);
    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 59, "287082", &last));
    TEST_ASSERT_EQUAL_UINT32(1u, last);
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "287082", &last));   // the same code again
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "755224", &last));   // an older one
    TEST_ASSERT_EQUAL_UINT32(1u, last);
    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 59, "359152", &last));    // one step ahead
    TEST_ASSERT_EQUAL_UINT32(2u, last);
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 89, "359152", &last));   // its own step, now spent

    // A failure leaves last as it was
    last = 7;
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "000000", &last));
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 59, "287082", &last));
    TEST_ASSERT_EQUAL_UINT32(7u, last);

    // RFC 6238 vector at 1111111109 (step 37037036): 8 digits 07081804
    char code[8];
    Totp::codeAt(RFC_KEY, 1111111109, code);
    TEST_ASSERT_EQUAL_STRING("081804", code);
    last = 37037035;
    TEST_ASSERT_TRUE(Totp::verify(RFC_KEY, 1111111109, "081804", &last));
    TEST_ASSERT_EQUAL_UINT32(37037036u, last);
    TEST_ASSERT_FALSE(Totp::verify(RFC_KEY, 1111111109, "081804", &last));
}

void test_random_key_is_never_blank() {
    uint8_t a[Totp::KEY_LEN], b[Totp::KEY_LEN];
    Totp::randomKey(a);
    Totp::randomKey(b);
    TEST_ASSERT_FALSE(Totp::blank(a));
    TEST_ASSERT_FALSE(Totp::blank(b));
    TEST_ASSERT_EQUAL_UINT8(1, a[0] & 1);
    TEST_ASSERT_TRUE(std::memcmp(a, b, Totp::KEY_LEN) != 0);

    char hex[41], b32[33];
    Totp::toHex(a, hex);
    Totp::toBase32(a, b32);
    uint8_t back[Totp::KEY_LEN];
    TEST_ASSERT_TRUE(Totp::fromHex(hex, back));
    TEST_ASSERT_EQUAL_MEMORY(a, back, Totp::KEY_LEN);
    TEST_ASSERT_TRUE(Totp::fromBase32(b32, back));
    TEST_ASSERT_EQUAL_MEMORY(a, back, Totp::KEY_LEN);
    TEST_ASSERT_TRUE(Totp::blank(RFC_KEY) == false);
    uint8_t z[Totp::KEY_LEN] = {};
    TEST_ASSERT_TRUE(Totp::blank(z));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_rfc_hotp_and_totp);
    RUN_TEST(test_hex_codec);
    RUN_TEST(test_base32_codec);
    RUN_TEST(test_verify_accepts_one_step_either_side);
    RUN_TEST(test_verify_refuses_spent_steps);
    RUN_TEST(test_random_key_is_never_blank);
    return UNITY_END();
}
