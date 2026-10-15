// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <map>
#include <string>
#include <unity.h>
#include <ArduinoJson.h>
#include "../test_support.h"
#include "Json.h"

void setUp()    { resetMocks(); }
void tearDown() {}

// Members of the top-level object, by name
struct Seen {
    std::map<std::string, json::Value> v;
    static void add(void* ctx, const char* key, size_t len, const json::Value& val) {
        static_cast<Seen*>(ctx)->v[std::string(key, len)] = val;
    }
};

static Seen read(const char* text, bool ok = true) {
    Seen s;
    TEST_ASSERT_EQUAL(ok, json::object(text, Seen::add, &s));
    return s;
}

static std::string text(const json::Value& v) { return json::str(v).c_str(); }

// ── reading ──────────────────────────────────────────────────────────────────

void test_kinds() {
    Seen s = read(" {\"s\":\"a\" , \"i\":-12,\"n\":1.5,\"t\":true,\"f\":false,\"z\":null,"
                  "\"o\":{\"x\":[1,{}]},\"a\":[ ]}\n trailing");
    TEST_ASSERT_EQUAL(json::STRING, s.v["s"].kind);
    TEST_ASSERT_EQUAL(json::INTEGER, s.v["i"].kind);
    TEST_ASSERT_TRUE(s.v["i"].neg);
    TEST_ASSERT_EQUAL(12u, s.v["i"].mag);
    TEST_ASSERT_EQUAL(json::NUMBER, s.v["n"].kind);
    TEST_ASSERT_EQUAL(json::BOOL, s.v["t"].kind);
    TEST_ASSERT_EQUAL(1u, s.v["t"].mag);
    TEST_ASSERT_EQUAL(0u, s.v["f"].mag);
    TEST_ASSERT_EQUAL(json::NUL, s.v["z"].kind);
    TEST_ASSERT_EQUAL(json::OBJECT, s.v["o"].kind);
    TEST_ASSERT_EQUAL(json::ARRAY, s.v["a"].kind);
    TEST_ASSERT_EQUAL(json::BAD, json::first(s.v["a"]).kind);   // empty array
    TEST_ASSERT_EQUAL(8, (int)s.v.size());
    read("{}");
}

void test_numbers() {
    Seen s = read("{\"a\":4294967295,\"b\":4294967296,\"c\":1e3,\"d\":2E-1,\"e\":3e+2,\"f\":-0}");
    TEST_ASSERT_EQUAL(json::INTEGER, s.v["a"].kind);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, s.v["a"].mag);
    TEST_ASSERT_EQUAL(json::NUMBER, s.v["b"].kind);              // more than 32 bits
    TEST_ASSERT_EQUAL(json::NUMBER, s.v["c"].kind);
    TEST_ASSERT_EQUAL(json::NUMBER, s.v["d"].kind);
    TEST_ASSERT_EQUAL(json::NUMBER, s.v["e"].kind);
    uint32_t n = 7;
    TEST_ASSERT_TRUE(s.v["f"].fits(0, 1, n));                    // -0 is 0
    TEST_ASSERT_EQUAL_UINT32(0u, n);
}

void test_fits() {
    Seen s = read("{\"m\":-128,\"m2\":-129,\"p\":127,\"p2\":128,\"s\":\"1\"}");
    uint32_t n = 0;
    TEST_ASSERT_TRUE(s.v["m"].fits(-128, 127, n));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFF80u, n);                    // two's complement
    TEST_ASSERT_FALSE(s.v["m2"].fits(-128, 127, n));
    TEST_ASSERT_TRUE(s.v["p"].fits(-128, 127, n));
    TEST_ASSERT_FALSE(s.v["p2"].fits(-128, 127, n));
    TEST_ASSERT_FALSE(s.v["m"].fits(0, 255, n));                 // negative where none is allowed
    TEST_ASSERT_FALSE(s.v["s"].fits(0, 255, n));                 // text is no number
}

// Invalid text: false, and no callback at all (the whole object is checked first)
void test_invalid() {
    const char* bad[] = {
        "", "[1]", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":1 \"b\":2}", "{a:1}",
        "{\"a\":\"x}", "{\"a\":\"\\q\"}", "{\"a\":\"\\u12g4\"}", "{\"a\":\"\\", "{\"a\":-}",
        "{\"a\":1.}", "{\"a\":1e}", "{\"a\":1e+}", "{\"a\":tru}", "{\"a\":[1,]}", "{\"a\":[1 2]}",
        "{\"a\":[1}", "{\"a\":{\"b\":1]}", "{\"a\":nul}", "{\"a\":\"ok\",\"b\":x}",
        "{\"ab", "{\"a\":\"\\u 123\"}", "{\"a\":\"\\u:000\"}", "{\"a\":-,\"b\":1}",
    };
    for (const char* t : bad) {
        Seen s;
        TEST_ASSERT_FALSE_MESSAGE(json::object(t, Seen::add, &s), t);
        TEST_ASSERT_EQUAL_MESSAGE(0, (int)s.v.size(), t);
    }
}

// Nesting is bounded (stack): 10 levels pass, 11 do not
void test_depth_limit() {
    std::string ok = "{\"a\":" + std::string(8, '[') + "1" + std::string(8, ']') + "}";
    TEST_ASSERT_TRUE(json::object(ok.c_str(), nullptr, nullptr));
    std::string deep = "{\"a\":" + std::string(9, '[') + "1" + std::string(9, ']') + "}";
    TEST_ASSERT_TRUE(json::object(deep.c_str(), nullptr, nullptr));
    std::string over = "{\"a\":" + std::string(10, '[') + "1" + std::string(10, ']') + "}";
    TEST_ASSERT_FALSE(json::object(over.c_str(), nullptr, nullptr));
}

// The same verdict as ArduinoJson on what the device can meet
void test_agrees_with_arduinojson() {
    const char* cases[] = {
        "{\"v\":2,\"ssid\":\"a\\\"b\",\"x\":[1,2,{\"y\":null}]}", "{\"a\":\"\\u00e9\\ud83d\\ude00\"}",
        "{\"a\":1,}", "{\"a\":[}", "{\"a\":\"x", "{\"success\":false,\"errors\":[{\"code\":1}]}",
        "{\"a\":-5,\"b\":true,\"c\":false,\"d\":null}", "{\"a\" : { } , \"b\" : [ ] }",
    };
    for (const char* t : cases) {
        DynamicJsonDocument doc(1024);
        bool theirs = !deserializeJson(doc, t) && doc.is<JsonObject>();
        TEST_ASSERT_EQUAL_MESSAGE(theirs, json::object(t, nullptr, nullptr), t);
    }
}

void test_nested_members_and_first() {
    Seen s = read("{\"e\":[{\"code\":7,\"message\":\"m\"},2],\"r\":[\"one\",\"two\"]}");
    json::Value e = json::first(s.v["e"]);
    TEST_ASSERT_EQUAL(json::OBJECT, e.kind);
    Seen inner;
    json::members(e, Seen::add, &inner);
    TEST_ASSERT_EQUAL(7u, inner.v["code"].mag);
    TEST_ASSERT_EQUAL_STRING("m", text(inner.v["message"]).c_str());
    TEST_ASSERT_EQUAL_STRING("one", text(json::first(s.v["r"])).c_str());
}

void test_is() {
    Seen s = read("{\"name\":1}");
    TEST_ASSERT_EQUAL(1, (int)s.v.count("name"));
    const char* key = "name\"";
    TEST_ASSERT_TRUE(json::is(key, 4, "name"));
    TEST_ASSERT_FALSE(json::is(key, 4, "nam"));
    TEST_ASSERT_FALSE(json::is(key, 4, "nome"));
}

// ── strings ──────────────────────────────────────────────────────────────────

void test_escapes_and_unicode() {
    Seen s = read("{\"a\":\"q\\\"b\\\\s\\/ \\b\\f\\n\\r\\t\",\"u\":\"\\u0041\\u00e9\\u20ac\\ud83d\\ude00\","
                  "\"lone\":\"\\ud83d!\",\"lone2\":\"\\ud83d\\u0041\"}");
    TEST_ASSERT_EQUAL_STRING("q\"b\\s/ \b\f\n\r\t", text(s.v["a"]).c_str());
    TEST_ASSERT_EQUAL_STRING("A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", text(s.v["u"]).c_str());
    // A lone high surrogate stays as its own 3-byte sequence
    TEST_ASSERT_EQUAL_STRING("\xed\xa0\xbd!", text(s.v["lone"]).c_str());
    TEST_ASSERT_EQUAL_STRING("\xed\xa0\xbd" "A", text(s.v["lone2"]).c_str());

    // A low surrogate alone, a high one before another escape or past the range
    Seen t = read("{\"lo\":\"\\udc00\",\"esc\":\"\\ud83d\\n\",\"hi\":\"\\ud83d\\ue000\"}");
    TEST_ASSERT_EQUAL_STRING("\xed\xb0\x80", text(t.v["lo"]).c_str());
    TEST_ASSERT_EQUAL_STRING("\xed\xa0\xbd\n", text(t.v["esc"]).c_str());
    TEST_ASSERT_EQUAL_STRING("\xed\xa0\xbd\xee\x80\x80", text(t.v["hi"]).c_str());
    // Two-byte characters and every kind of space between tokens
    Seen u = read("{\r\n\t\"x\" :\t\"\\u00ff\\u07ff\" }");
    TEST_ASSERT_EQUAL_STRING("\xc3\xbf\xdf\xbf", text(u.v["x"]).c_str());
}

void test_copy_cuts_and_reports_the_length() {
    Seen s = read("{\"a\":\"abcdef\",\"e\":\"\\u00e9\\u00e9\"}");
    char b[4];
    TEST_ASSERT_EQUAL(6, (int)json::copy(s.v["a"], b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("abc", b);
    char big[16];
    TEST_ASSERT_EQUAL(6, (int)json::copy(s.v["a"], big, sizeof(big)));
    TEST_ASSERT_EQUAL_STRING("abcdef", big);
    TEST_ASSERT_EQUAL(4, (int)json::copy(s.v["e"], b, sizeof(b)));   // bytes after decoding
}

// ── writing ──────────────────────────────────────────────────────────────────

void test_writer_to_a_file_and_measuring() {
    File f("", true, "/x.json");
    json::Writer w(&f);
    w.str("s", "a\"b\\c\x01\x1f\xc3\xa9");
    w.num("n", -42);
    w.unum("u", 4294967295u);
    w.boolean("t", true);
    w.boolean("f", false);
    size_t n = w.end();
    f.close();
    const std::string& out = MockFS::files["/x.json"];
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"a\\\"b\\\\c\\u0001\\u001f\xc3\xa9\",\"n\":-42,\"u\":4294967295,"
                             "\"t\":true,\"f\":false}", out.c_str());
    TEST_ASSERT_EQUAL(out.size(), n);

    // Read back: the same values
    Seen s = read(out.c_str());
    TEST_ASSERT_EQUAL_STRING("a\"b\\c\x01\x1f\xc3\xa9", text(s.v["s"]).c_str());
    DynamicJsonDocument doc(512);
    TEST_ASSERT_FALSE(deserializeJson(doc, out.c_str()));
    TEST_ASSERT_EQUAL_STRING("a\"b\\c\x01\x1f\xc3\xa9", doc["s"].as<const char*>());

    json::Writer measure;
    measure.num("n", 5);
    TEST_ASSERT_EQUAL(7, (int)measure.end());                    // {"n":5}
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_kinds);
    RUN_TEST(test_numbers);
    RUN_TEST(test_fits);
    RUN_TEST(test_invalid);
    RUN_TEST(test_depth_limit);
    RUN_TEST(test_agrees_with_arduinojson);
    RUN_TEST(test_nested_members_and_first);
    RUN_TEST(test_is);
    RUN_TEST(test_escapes_and_unicode);
    RUN_TEST(test_copy_cuts_and_reports_the_length);
    RUN_TEST(test_writer_to_a_file_and_measuring);
    return UNITY_END();
}
