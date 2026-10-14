// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "ReverseDNS.h"

void setUp()    { resetMocks(); }
void tearDown() {}

static std::string bytes(const uint8_t* b, size_t n) { return std::string((const char*)b, n); }

static std::string ask(const char* ip, uint16_t id) {
    uint8_t q[48];
    size_t n = ReverseDNS::query(ip, id, q, sizeof(q));
    return bytes(q, n);
}

static std::string label(const std::string& name) {
    std::string out;
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        out += (char)(dot - start);
        out += name.substr(start, dot - start);
        start = dot + 1;
    }
    return out + '\0';
}

// Answer to a query: answer header + records
static std::string answer(const std::string& q, const std::string& records, uint16_t count,
                          uint8_t rcode = 0) {
    std::string r = q;
    r[2] = (char)0x81;
    r[3] = (char)(0x80 | rcode);
    r[6] = (char)(count >> 8);
    r[7] = (char)count;
    return r + records;
}

// Record whose name points to the query (0xC00C)
static std::string record(uint16_t type, const std::string& rdata) {
    std::string r = {(char)0xC0, 0x0C, 0, (char)type, 0, 1, 0, 0, 0x0E, 0x10};
    r += (char)(rdata.size() >> 8);
    r += (char)rdata.size();
    return r + rdata;
}

static std::string parsed(const std::string& msg, uint16_t id, size_t len = 64) {
    char out[64] = "junk";
    bool ok = ReverseDNS::parse((const uint8_t*)msg.data(), msg.size(), id, out, len);
    return ok ? std::string(out) : std::string("!") + out;
}

// ── query ────────────────────────────────────────────────────────────────────

void test_ptr_query() {
    std::string q = ask("200.10.5.1", 0x1234);
    std::string expect = std::string("\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00", 12) +
                         label("1.5.10.200.in-addr.arpa") + std::string("\x00\x0c\x00\x01", 4);
    TEST_ASSERT_TRUE(q == expect);

    TEST_ASSERT_EQUAL(46, (int)ask("255.255.255.255", 1).size());   // largest possible
    uint8_t small[45];
    TEST_ASSERT_EQUAL(0, (int)ReverseDNS::query("1.2.3.4", 1, small, sizeof(small)));
    TEST_ASSERT_EQUAL(0, (int)ask("1.2.3", 1).size());
}

// ── answer ───────────────────────────────────────────────────────────────────

void test_answer_with_name() {
    std::string q = ask("1.2.3.4", 7);
    std::string r = answer(q, record(12, label("host-1-2-3-4.isp.example.net")), 1);
    TEST_ASSERT_EQUAL_STRING("host-1-2-3-4.isp.example.net", parsed(r, 7).c_str());
}

// Another record first (delegation CNAME) and a compressed name in the PTR
void test_skips_other_type_and_follows_pointer() {
    std::string q = ask("1.2.3.4", 7);
    std::string ptr = std::string("\x04host", 5) + std::string("\xC0\x14", 2);  // + "in-addr.arpa"
    std::string r = answer(q, record(5, label("x.y")) + record(12, ptr), 2);
    TEST_ASSERT_EQUAL((char)7, q[0x14]);   // 0x14: the "in-addr" label in the query
    TEST_ASSERT_EQUAL_STRING("host.in-addr.arpa", parsed(r, 7).c_str());
}

void test_rejected_answer() {
    std::string q = ask("1.2.3.4", 7);
    std::string ok = answer(q, record(12, label("a.b")), 1);
    TEST_ASSERT_EQUAL_STRING("a.b", parsed(ok, 7).c_str());

    TEST_ASSERT_EQUAL_STRING("!junk", parsed(ok.substr(0, 11), 7).c_str());     // short
    TEST_ASSERT_EQUAL_STRING("!junk", parsed(ok, 8).c_str());                   // another query
    TEST_ASSERT_EQUAL_STRING("!junk", parsed(q, 7).c_str());                    // not an answer
    TEST_ASSERT_EQUAL_STRING("!junk", parsed(answer(q, "", 0, 3), 7).c_str());  // NXDOMAIN
    TEST_ASSERT_EQUAL_STRING("!junk", parsed(answer(q, "", 0), 7).c_str());     // no records
    TEST_ASSERT_EQUAL_STRING("!junk", parsed(answer(q, record(1, "\x01\x02\x03\x04"), 1), 7).c_str());
    // Cut in the middle of the record and in the middle of the data
    TEST_ASSERT_EQUAL_STRING("!junk", parsed(ok.substr(0, q.size() + 6), 7).c_str());
    TEST_ASSERT_EQUAL_STRING("!junk", parsed(ok.substr(0, ok.size() - 1), 7).c_str());
    // Query name with no end
    std::string open = q.substr(0, 14);
    open[5] = 1;
    open[7] = 1;
    TEST_ASSERT_EQUAL_STRING("!junk", parsed(answer(open, "", 1).substr(0, 14), 7).c_str());
}

void test_malformed_name() {
    std::string q = ask("1.2.3.4", 7);
    auto with = [&](const std::string& rdata) { return parsed(answer(q, record(12, rdata), 1), 7); };
    TEST_ASSERT_EQUAL_STRING("!", with(std::string("\0", 1)).c_str());                 // empty
    TEST_ASSERT_EQUAL_STRING("!", with(std::string("\x05" "ab", 3)).c_str());          // label past the end
    TEST_ASSERT_EQUAL_STRING("!", with(std::string("\x02" "ab\xC0", 4)).c_str());      // truncated pointer
    TEST_ASSERT_EQUAL_STRING("!", with(std::string("\x02" "ab", 3)).c_str());          // no terminator
    // Pointer to itself: stops after 8 jumps
    std::string loop = answer(q, record(12, ""), 1);
    size_t at = loop.size();
    loop += (char)(0xC0 | (at >> 8));
    loop += (char)at;
    loop[loop.size() - 3] = 2;   // rdlen = 2
    TEST_ASSERT_EQUAL_STRING("!", parsed(loop, 7).c_str());
    // Does not fit in the output
    std::string r = answer(q, record(12, label("abcdef.gh")), 1);
    TEST_ASSERT_EQUAL_STRING("!", parsed(r, 7, 8).c_str());
    TEST_ASSERT_EQUAL_STRING("abcdef.gh", parsed(r, 7, 10).c_str());
}

// ── UDP ──────────────────────────────────────────────────────────────────────

void test_asks_network_dns_and_reads_answer() {
    ReverseDNS r;
    char host[64] = "previous";
    r.lookup("1.2.3.4", host, sizeof(host), 100);
    TEST_ASSERT_EQUAL_STRING("", host);
    TEST_ASSERT_TRUE(r.pending());
    TEST_ASSERT_EQUAL(0x0101A8C0u, MockUDP::to);
    TEST_ASSERT_EQUAL(53, MockUDP::port);
    TEST_ASSERT_TRUE(MockUDP::sent == ask("1.2.3.4", 1));
    TEST_ASSERT_EQUAL(0, MockUDP::pbufs);

    MockUDP::deliver(answer(MockUDP::sent, record(12, label("home.isp.net")), 1));
    TEST_ASSERT_EQUAL_STRING("home.isp.net", host);
    TEST_ASSERT_FALSE(r.pending());
    TEST_ASSERT_EQUAL(0, MockUDP::live);
    TEST_ASSERT_EQUAL(0, MockUDP::pbufs);
}

// One query at a time; the previous one is dropped
void test_new_query_and_answer_in_pieces() {
    ReverseDNS r;
    char host[64];
    r.lookup("1.2.3.4", host, sizeof(host), 0);
    r.lookup("5.6.7.8", host, sizeof(host), 0);
    TEST_ASSERT_EQUAL(1, MockUDP::live);
    TEST_ASSERT_TRUE(MockUDP::sent == ask("5.6.7.8", 2));

    MockUDP::deliver(answer(MockUDP::sent, record(12, label("a.b")), 1), 10);
    TEST_ASSERT_EQUAL_STRING("", host);
    TEST_ASSERT_FALSE(r.pending());
    TEST_ASSERT_EQUAL(0, MockUDP::pbufs);
}

void test_no_answer_gives_up() {
    ReverseDNS r;
    char host[64];
    r.lookup("1.2.3.4", host, sizeof(host), 100);
    r.tick(100 + ReverseDNS::TIMEOUT - 1);
    TEST_ASSERT_TRUE(r.pending());
    r.tick(100 + ReverseDNS::TIMEOUT);
    TEST_ASSERT_FALSE(r.pending());
    r.tick(100 + 2 * ReverseDNS::TIMEOUT);
    TEST_ASSERT_EQUAL(0, MockUDP::live);
}

void test_does_not_ask_without_conditions() {
    char host[64];
    {
        ReverseDNS r;
        r.lookup("1.2.3", host, sizeof(host), 0);   // invalid IP
        WiFi._dns = IPAddress(0);                   // no network DNS
        r.lookup("1.2.3.4", host, sizeof(host), 0);
        TEST_ASSERT_TRUE(MockUDP::sent.empty());
        WiFi._dns = IPAddress(0x0101A8C0);

        MockUDP::failAlloc = true;
        r.lookup("1.2.3.4", host, sizeof(host), 0);
        MockUDP::failAlloc = false;
        MockUDP::failNew = true;
        r.lookup("1.2.3.4", host, sizeof(host), 0);
        TEST_ASSERT_FALSE(r.pending());
        TEST_ASSERT_TRUE(MockUDP::sent.empty());
        TEST_ASSERT_EQUAL(0, MockUDP::pbufs);
        MockUDP::failNew = false;

        r.lookup("1.2.3.4", host, sizeof(host), 0);
        TEST_ASSERT_EQUAL(1, MockUDP::live);
    }
    TEST_ASSERT_EQUAL(0, MockUDP::live);   // destructor closes
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ptr_query);
    RUN_TEST(test_answer_with_name);
    RUN_TEST(test_skips_other_type_and_follows_pointer);
    RUN_TEST(test_rejected_answer);
    RUN_TEST(test_malformed_name);
    RUN_TEST(test_asks_network_dns_and_reads_answer);
    RUN_TEST(test_new_query_and_answer_in_pieces);
    RUN_TEST(test_no_answer_gives_up);
    RUN_TEST(test_does_not_ask_without_conditions);
    return UNITY_END();
}
