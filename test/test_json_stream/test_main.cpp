// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include <string>
#include <vector>
#include "../test_support.h"
#include "JsonStream.h"

void setUp()    { resetMocks(); }
void tearDown() {}

// Every call as "depth path=value" (path: the keys joined by /), "}" for an object's end
struct Log {
    std::vector<std::string> calls;
    bool lastCut = false;

    static void fn(void* ctx, const JsonStream& s, const char* v) {
        Log& l = *static_cast<Log*>(ctx);
        std::string path;
        for (uint8_t i = 1; i <= s.depth() && i <= JsonStream::MAX_DEPTH; i++) {
            if (i > 1) path += "/";
            path += s.key(i);
        }
        l.calls.push_back(std::to_string(s.depth()) + " " + path + (v ? std::string("=") + v : std::string(" }")));
        l.lastCut = s.cut();
    }
};

static Log run(const std::string& text) {
    Log l;
    JsonStream js(Log::fn, &l);
    for (char c : text) js.feed(c);
    return l;
}

static std::string joined(const Log& l) {
    std::string out;
    for (auto& c : l.calls) out += c + "\n";
    return out;
}

void test_cloudflare_shape() {
    Log l = run(R"({"result":[{"id":"z1","name":"a.com","account":{"id":"acc","name":"Me"}},)"
                R"({"id":"z2","name":"b.com"}],"success":true,"errors":[],"count":2})");
    TEST_ASSERT_EQUAL_STRING(
        "3 result//id=z1\n"
        "3 result//name=a.com\n"
        "4 result//account/id=acc\n"
        "4 result//account/name=Me\n"
        "4 result//account/name }\n"
        "3 result//account }\n"
        "3 result//id=z2\n"
        "3 result//name=b.com\n"
        "3 result//name }\n"
        "1 success=true\n"
        "1 count=2\n"
        "1 count }\n",
        joined(l).c_str());
}

void test_literals_numbers_and_spaces() {
    Log l = run("{ \"a\" : null ,\n\t\"b\": -1.5e3\r\n, \"c\":false}");
    TEST_ASSERT_EQUAL_STRING("1 a=null\n1 b=-1.5e3\n1 c=false\n1 c }\n", joined(l).c_str());
}

void test_escapes() {
    Log l = run(R"({"m":"a\"b\\c\/d\)" "u00e9!" R"(","k\"ey":1})");
    TEST_ASSERT_EQUAL_STRING("1 m=a\"b\\c/d?!\n1 k\"ey=1\n1 k\"ey }\n", joined(l).c_str());
}

void test_long_text_is_cut_and_said() {
    std::string big(JsonStream::VALUE_LEN + 10, 'x');
    Log l = run("{\"a\":\"" + big + "\"");
    TEST_ASSERT_EQUAL(1, (int)l.calls.size());
    TEST_ASSERT_TRUE(l.lastCut);
    TEST_ASSERT_EQUAL((int)JsonStream::VALUE_LEN - 1, (int)l.calls[0].size() - (int)std::string("1 a=").size());

    std::string num(JsonStream::VALUE_LEN + 5, '7');
    Log n = run("{\"a\":" + num + ",\"b\":1}");
    TEST_ASSERT_TRUE(n.calls.size() == 3);
    TEST_ASSERT_FALSE(n.lastCut);            // the next value starts afresh

    // A long key is cut to KEY_LEN - 1
    Log k = run("{\"abcdefghijklmnopq\":1}");
    TEST_ASSERT_EQUAL_STRING("1 abcdefghijk=1\n1 abcdefghijk }\n", joined(k).c_str());
}

// Too deep: nothing reported down there, the levels above still are
void test_depth_limit() {
    Log l = run(R"({"a":{"b":{"c":{"d":{"e":{"f":1,"g":"x"}},"h":2}}},"i":3})");
    TEST_ASSERT_EQUAL_STRING(
        "5 a/b/c/d/e }\n"
        "4 a/b/c/h=2\n"
        "4 a/b/c/h }\n"
        "3 a/b/c }\n"
        "2 a/b }\n"
        "1 i=3\n"
        "1 i }\n",
        joined(l).c_str());
}

void test_arrays_have_no_keys() {
    Log l = run(R"([1,"two",[true],{"k":[{"x":0}]}])");
    TEST_ASSERT_EQUAL_STRING(
        "1 =1\n"
        "1 =two\n"
        "2 /=true\n"
        "4 /k//x=0\n"
        "4 /k//x }\n"
        "2 /k }\n",
        joined(l).c_str());
    TEST_ASSERT_EQUAL_STRING("", JsonStream(Log::fn, nullptr).key(0));
    TEST_ASSERT_EQUAL_STRING("", JsonStream(Log::fn, nullptr).key(JsonStream::MAX_DEPTH + 1));
}

// Junk never faults: stray closings, values at the top, endless nesting
void test_junk() {
    Log a = run("}}]]\"top\" 12 ,:");
    TEST_ASSERT_EQUAL(0, (int)a.calls.size());
    std::string deep(300, '[');
    Log b = run(deep + "1" + std::string(300, ']') + "{\"z\":1}");
    TEST_ASSERT_TRUE(b.calls.size() >= 1);
    Log c = run(std::string(300, '{') + "\"k\":1");
    TEST_ASSERT_EQUAL(0, (int)c.calls.size());
    Log d = run(std::string(10, '[') + "1,2");      // commas below the tracked levels
    TEST_ASSERT_EQUAL(0, (int)d.calls.size());
    Log e = run("{,}\"x\":1");                       // a key after the end
    TEST_ASSERT_EQUAL(1, (int)e.calls.size());
}

void test_is_compares_the_key_at_a_level() {
    struct Ctx { bool hit = false; } ctx;
    JsonStream js([](void* c, const JsonStream& s, const char* v) {
        if (v && s.is(1, PSTR("result")) && s.is(2, PSTR("id"))) static_cast<Ctx*>(c)->hit = true;
    }, &ctx);
    for (char ch : std::string(R"({"result":{"id":"x"}})")) js.feed(ch);
    TEST_ASSERT_TRUE(ctx.hit);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_cloudflare_shape);
    RUN_TEST(test_literals_numbers_and_spaces);
    RUN_TEST(test_escapes);
    RUN_TEST(test_long_text_is_cut_and_said);
    RUN_TEST(test_depth_limit);
    RUN_TEST(test_arrays_have_no_keys);
    RUN_TEST(test_junk);
    RUN_TEST(test_is_compares_the_key_at_a_level);
    return UNITY_END();
}
