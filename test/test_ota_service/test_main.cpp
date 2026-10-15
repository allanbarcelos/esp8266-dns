// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "WiFiService.h"
#include "OTAService.h"

static Logger logger;
static Config cfg;

void setUp()    { resetMocks(); logger = Logger(); cfg = Config{}; }
void tearDown() {}

struct Fixture {
    WiFiService wifi{cfg, logger};
    OTAService  ota{logger, wifi};
};

static const unsigned long T = OTAService::CHECK_INTERVAL;

// Queues a version check + a download of `size` bytes
static void queueUpdate(const std::string& body, int size = -1) {
    MockHTTP::push(200, R"({"tag_name":"v999","name":"x"})");
    MockHTTP::push(200, body, size);
}

// ── version ──────────────────────────────────────────────────────────────────

void test_parse_tag() {
    TEST_ASSERT_EQUAL(42, OTAService::parseTag("v42"));
    TEST_ASSERT_EQUAL(0,  OTAService::parseTag("v0"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("42"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("v"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("v4a"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("v1.2"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("dev"));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag(nullptr));
    TEST_ASSERT_EQUAL(-1, OTAService::parseTag("v99999999999"));
}

void test_is_newer() {
    TEST_ASSERT_TRUE (OTAService::isNewer("v2", "v1"));
    TEST_ASSERT_FALSE(OTAService::isNewer("v1", "v1"));
    TEST_ASSERT_FALSE(OTAService::isNewer("v1", "v2"));   // no downgrade
    TEST_ASSERT_FALSE(OTAService::isNewer("junk", "v1"));
    TEST_ASSERT_TRUE (OTAService::isNewer("v1", "dev"));  // local build
}

// ── scheduling ───────────────────────────────────────────────────────────────

void test_no_check_before_interval() {
    Fixture f;
    f.ota.tick(0);
    f.ota.tick(OTAService::FIRST_CHECK - 1);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_first_automatic_check() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.tick(OTAService::FIRST_CHECK);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs[0], "releases/latest"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_check_after_interval() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs[0], "releases/latest"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_no_check_after_deadline() {
    Fixture f;
    f.ota.tick(OTAService::DEADLINE);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_no_check_with_restart_pending() {
    Fixture f;
    f.wifi.scheduleRestart(1000);
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_no_check_without_wifi() {
    Fixture f;
    WiFi._status = WL_DISCONNECTED;
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

// ── version check ────────────────────────────────────────────────────────────

void test_api_connection_fails() {
    Fixture f;
    MockTLS::failConnect = true;
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Version check failed"));
}

void test_api_http_error() {
    Fixture f;
    MockHTTP::push(403);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Version HTTP error: 403"));
}

void test_api_invalid_json() {
    Fixture f;
    MockHTTP::push(200, "{not json");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Version JSON error"));
}

void test_empty_tag() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"  "})");
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

// ── download ─────────────────────────────────────────────────────────────────

void test_starts_ota_with_the_build_file() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);

    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1024, f.ota.contentLength());
    TEST_ASSERT_EQUAL(0, (int)f.ota.bytesWritten());
    std::string url = "https://github.com/allanbarcelos/esp8266-dns/releases/download/v999/";
    url += OTAService::assetName(OTAService::signingEnabled());
    TEST_ASSERT_EQUAL_STRING(url.c_str(), MockHTTP::calledURLs[1].c_str());
    TEST_ASSERT_EQUAL(1024, (int)Update._size);
}

// Gzip of 32 bytes 0xE9 0x00…, mtime 0. The last 4 bytes are the
// uncompressed size eboot will copy.
static const uint8_t kGzip[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x7b, 0xc9,
    0x80, 0x1f, 0x00, 0x00, 0x6d, 0x75, 0xd8, 0xae, 0x20, 0x00, 0x00, 0x00
};

static uint32_t ebootSize(const uint8_t* image, size_t n) {
    TEST_ASSERT_TRUE(n >= 6);
    TEST_ASSERT_EQUAL_HEX8(0x1f, image[0]);
    TEST_ASSERT_EQUAL_HEX8(0x8b, image[1]);
    return (uint32_t)image[n - 4]
         | ((uint32_t)image[n - 3] << 8)
         | ((uint32_t)image[n - 2] << 16)
         | ((uint32_t)image[n - 1] << 24);
}

// The same cut Update.end() makes when verification is on: the trailing
// u32 = signature length, which does not go into the boot image.
static bool stripSignature(const uint8_t* image, size_t n, size_t& payload) {
    if (n < 4) return false;
    uint32_t sigLen = (uint32_t)image[n - 4]
                    | ((uint32_t)image[n - 3] << 8)
                    | ((uint32_t)image[n - 2] << 16)
                    | ((uint32_t)image[n - 1] << 24);
    if (sigLen == 0 || sigLen > n - 4) return false;
    payload = n - 4 - sigLen;
    return payload >= 6;
}

void test_file_matches_eboot() {
    TEST_ASSERT_EQUAL_STRING("firmware.tagged.bin", OTAService::assetName(true));
    TEST_ASSERT_EQUAL_STRING("firmware.bin", OTAService::assetName(false));
    TEST_ASSERT_EQUAL(32, (int)ebootSize(kGzip, sizeof(kGzip)));

    const uint8_t sig[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t signedImg[sizeof(kGzip) + sizeof(sig) + 4];
    std::memcpy(signedImg, kGzip, sizeof(kGzip));
    std::memcpy(signedImg + sizeof(kGzip), sig, sizeof(sig));
    signedImg[sizeof(kGzip) + sizeof(sig) + 0] = (uint8_t)sizeof(sig);
    signedImg[sizeof(kGzip) + sizeof(sig) + 1] = 0;
    signedImg[sizeof(kGzip) + sizeof(sig) + 2] = 0;
    signedImg[sizeof(kGzip) + sizeof(sig) + 3] = 0;

    // Without removing the trailer, eboot would copy 8 bytes, not the firmware.
    TEST_ASSERT_EQUAL((int)sizeof(sig), (int)ebootSize(signedImg, sizeof(signedImg)));

    size_t payload = 0;
    TEST_ASSERT_TRUE(stripSignature(signedImg, sizeof(signedImg), payload));
    TEST_ASSERT_EQUAL(sizeof(kGzip), payload);
    TEST_ASSERT_EQUAL(32, (int)ebootSize(signedImg, payload));

    size_t ignored = 0;
    uint8_t curto[] = {1, 2, 3};
    TEST_ASSERT_FALSE(stripSignature(curto, sizeof(curto), ignored));
    uint8_t semAssinatura[] = {0, 0, 0, 0};
    TEST_ASSERT_FALSE(stripSignature(semAssinatura, sizeof(semAssinatura), ignored));
    uint8_t grande[] = {0x1f, 0x8b, 0, 0, 5, 0, 0, 0};
    TEST_ASSERT_FALSE(stripSignature(grande, sizeof(grande), ignored));

    const char* asset = OTAService::assetName(OTAService::signingEnabled());
    if (OTAService::signingEnabled()) {
        TEST_ASSERT_EQUAL_STRING("firmware.tagged.bin", asset);
        TEST_ASSERT_EQUAL(32, (int)ebootSize(signedImg, payload));
    } else {
        TEST_ASSERT_EQUAL_STRING("firmware.bin", asset);
        TEST_ASSERT_EQUAL(32, (int)ebootSize(kGzip, sizeof(kGzip)));
    }
}

// SHA-256 standing in for the core's HashSHA256 (FIPS 180-4), to check the
// tagged digest against the same value package_ota.py signs
struct Sha256 : UpdaterHashClass {
    uint32_t h[8];
    uint8_t  buf[64];
    uint64_t bytes = 0;
    uint8_t  out[32];
    int      ends = 0;

    void begin() override {
        static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h, H0, sizeof(h));
        bytes = 0;
    }
    void add(const void* data, uint32_t len) override {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0; i < len; i++) {
            buf[bytes++ % 64] = p[i];
            if (bytes % 64 == 0) block();
        }
    }
    void end() override {
        uint64_t bits = bytes * 8;
        uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (bytes % 64 != 56) add(&zero, 1);
        for (int i = 7; i >= 0; i--) {
            uint8_t b = (uint8_t)(bits >> (8 * i));
            add(&b, 1);
        }
        for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
        ends++;
    }
    int len() override { return 32; }
    const void* hash() override { return out; }
    const unsigned char* oid() override { return out; }

    static uint32_t rot(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block() {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)buf[4 * i] << 24 | (uint32_t)buf[4 * i + 1] << 16 |
                   (uint32_t)buf[4 * i + 2] << 8 | buf[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = k + (rot(e, 6) ^ rot(e, 11) ^ rot(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rot(a, 2) ^ rot(a, 13) ^ rot(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }
};

static std::string hex(const void* p, int n) {
    std::string s;
    char b[3];
    for (int i = 0; i < n; i++) {
        std::snprintf(b, sizeof(b), "%02x", static_cast<const uint8_t*>(p)[i]);
        s += b;
    }
    return s;
}

// The signature covers SHA-256(SHA-256(image) || tag): the same image under
// another tag gives another digest, so an older release cannot pass as a newer one
void test_tagged_hash_binds_the_version() {
    Sha256 sha;
    sha.begin();
    sha.add("abc", 3);
    sha.end();
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                             hex(sha.hash(), 32).c_str());
    std::string hundred(100, 'a');                                   // more than one block
    sha.begin();
    sha.add(hundred.data(), 100);
    sha.end();
    TEST_ASSERT_EQUAL_STRING("2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e",
                             hex(sha.hash(), 32).c_str());

    TaggedHash tagged(sha);
    tagged.tag("v42");
    tagged.begin();
    tagged.add("pay", 3);
    tagged.add("load", 4);
    tagged.end();
    // python: sha256(sha256(b"payload").digest() + b"v42")
    TEST_ASSERT_EQUAL_STRING("147160d5a63ca021e163efd6d6d807997b3098fccab8d6d17e7507b4540e4a48",
                             hex(tagged.hash(), tagged.len()).c_str());
    TEST_ASSERT_EQUAL_PTR(sha.oid(), tagged.oid());

    tagged.tag("v43");
    tagged.begin();
    tagged.add("payload", 7);
    tagged.end();
    TEST_ASSERT_NOT_EQUAL(0, hex(tagged.hash(), 32).compare(
        "147160d5a63ca021e163efd6d6d807997b3098fccab8d6d17e7507b4540e4a48"));
}

void test_download_connection_fails() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockTLS::failOnConnect = 2;
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Could not start OTA"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_download_http_error() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(404);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA HTTP error: 404"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_invalid_size() {
    Fixture f;
    queueUpdate("");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Invalid firmware size"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_no_room() {
    Fixture f;
    Update._beginOk = false;
    queueUpdate(std::string(100, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "No room for OTA"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

// ── streaming ────────────────────────────────────────────────────────────────

void test_full_stream_schedules_restart() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);

    f.ota.tick(T + 1);
    TEST_ASSERT_EQUAL(512, (int)f.ota.bytesWritten());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: 50%"));
    TEST_ASSERT_TRUE(f.ota.inProgress());

    f.ota.tick(T + 2);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, Update._ends);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: 100%"));
    TEST_ASSERT_TRUE(logHas(logger, "OTA done"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_progress_logged_every_10_percent() {
    Fixture f;
    queueUpdate(std::string(512 * 40, 'X'));
    f.ota.tick(T);
    for (int i = 1; i <= 3; i++) f.ota.tick(T + i); // 3/40 = 7.5%
    TEST_ASSERT_FALSE(logHas(logger, "OTA: 0%"));
    f.ota.tick(T + 4);                               // 10%
    TEST_ASSERT_TRUE(logHas(logger, "OTA: 10%"));
}

void test_invalid_signature_does_not_restart() {
    Fixture f;
    Update._endOk = false;
    queueUpdate(std::string(100, 'X'));
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA error: Signature Verification Failed"));
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

void test_write_error_aborts() {
    Fixture f;
    Update._shortWrite = true;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, Update._aborts);
    TEST_ASSERT_TRUE(logHas(logger, "OTA write error"));
}

void test_wifi_drops_during_ota() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);
    WiFi._status = WL_DISCONNECTED;
    f.ota.tick(T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, Update._aborts);
    TEST_ASSERT_TRUE(logHas(logger, "Wi-Fi dropped during OTA"));
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_FALSE(f.wifi.restartPending());
}

void test_stalled_stream_aborts_on_timeout() {
    Fixture f;
    queueUpdate(std::string(512, 'X'), 4096); // the server promises 4096, sends 512
    f.ota.tick(T);
    f.ota.tick(T + 1);                         // consumes the 512 bytes
    f.ota.tick(T + 1 + OTAService::STALL_TIMEOUT);
    TEST_ASSERT_TRUE(f.ota.inProgress());      // still within the limit

    f.ota.tick(T + 2 + OTAService::STALL_TIMEOUT);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, Update._aborts);
    TEST_ASSERT_TRUE(logHas(logger, "OTA without data"));
}

void test_automatic_off_does_not_check() {
    Fixture f;
    f.ota.setAutomatic(false);
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.automatic());
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_manual_check_does_not_install() {
    Fixture f;
    f.ota.setAutomatic(false);
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    TEST_ASSERT_EQUAL(OTAService::Report::Checking, f.ota.report());
    f.ota.requestCheck(); // already pending: does not restart the request
    f.ota.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.ota.pending());
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
    TEST_ASSERT_EQUAL(OTAService::Report::Available, f.ota.report());
    TEST_ASSERT_TRUE(logHas(logger, "New version found: v999"));
}

void test_manual_check_already_up_to_date() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    TEST_ASSERT_EQUAL(OTAService::Report::UpToDate, f.ota.report());
    TEST_ASSERT_EQUAL_STRING("", f.ota.availableVersion());
    TEST_ASSERT_EQUAL_STRING("Firmware is up to date", f.ota.detail());
}

void test_manual_install() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.requestInstall();
    TEST_ASSERT_TRUE(f.ota.pending());
    f.ota.tick(1);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(128, f.ota.contentLength());
    TEST_ASSERT_TRUE(contains(
        MockHTTP::calledURLs.back(),
        OTAService::assetName(OTAService::signingEnabled())));
}

void test_install_without_version_is_ignored() {
    Fixture f;
    f.ota.setAutomatic(false);
    f.ota.requestInstall();
    TEST_ASSERT_FALSE(f.ota.pending());
    TEST_ASSERT_EQUAL(OTAService::Report::None, f.ota.report());
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
}

void test_requests_ignored_during_download() {
    Fixture f;
    Update._endOk = false;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    f.ota.requestCheck();
    f.ota.requestInstall();
    f.ota.tick(T + 1);
    f.ota.tick(T + 2);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.wifi.restartPending());
    int calls = (int)MockHTTP::calledURLs.size();
    f.ota.tick(T + 3);
    TEST_ASSERT_EQUAL(calls, (int)MockHTTP::calledURLs.size());
}

void test_wifi_drops_with_request_pending() {
    Fixture f;
    f.ota.requestCheck();
    WiFi._status = WL_DISCONNECTED;
    f.ota.tick(0);
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());

    WiFi._status = WL_CONNECTED;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(1);
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
    f.ota.requestInstall();
    WiFi._status = WL_DISCONNECTED;
    f.ota.tick(2);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_check_not_enough_memory() {
    Fixture f;
    ESP._heap = OTAService::GITHUB_HEAP - 1;
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: not enough memory"));
    TEST_ASSERT_TRUE(logHas(logger, "OTA heap"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(LittleFS.exists("/ota.pending"));
}

void test_check_block_too_small() {
    Fixture f;
    ESP._maxBlock = OTAService::GITHUB_BLOCK - 1;
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: not enough memory"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_check_tls_unavailable() {
    MockTLS::nullOwned = true;
    Fixture f;
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_EQUAL_STRING("Retrying in 10 s", f.ota.detail());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: TLS unavailable"));
}

void test_download_not_enough_memory() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    ESP._heap = OTAService::GITHUB_HEAP - 1;
    f.ota.requestInstall();
    f.ota.tick(1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: not enough memory"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(LittleFS.exists("/ota.pending"));
}

void test_download_tls_unavailable() {
    Fixture f;
    MockTLS::failOnConnect = 2;
    queueUpdate(std::string(64, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
    TEST_ASSERT_EQUAL_STRING("Retrying in 10 s", f.ota.detail());
    TEST_ASSERT_TRUE(logHas(logger, "New version found"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
}

void test_no_new_check_during_ota() {
    Fixture f;
    queueUpdate(std::string(512, 'X'), 4096);
    f.ota.tick(T);
    f.ota.tick(T + 1);
    size_t before = MockHTTP::calledURLs.size();
    f.ota.tick(T + 2);
    TEST_ASSERT_EQUAL((int)before, (int)MockHTTP::calledURLs.size());
}

void test_success_deletes_request() {
    MockFS::put("/ota.pending", "v999 1");
    Fixture f;
    f.ota.begin();
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.tick(0);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    f.ota.tick(1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "OTA done"));
}

void test_github_uses_large_tls_buffer() {
    Fixture f;
    queueUpdate(std::string(1024, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(16384, MockTLS::bufIn);
    TEST_ASSERT_EQUAL(512, MockTLS::bufOut);
    TEST_ASSERT_TRUE(MockHTTP::lastHTTP10);
    TEST_ASSERT_EQUAL_STRING("ESP8266-dns", MockHTTP::lastUA.c_str());
}

void test_retries_after_backoff() {
    Fixture f;
    MockHTTP::push(403);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    f.ota.tick(T + OTAService::RETRY_MS - 1);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(T + OTAService::RETRY_MS);
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_unknown_size_completes_at_end_of_stream() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(600, 'Y'), -2);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(0, f.ota.progressPercent());
    f.ota.tick(T + 1);
    f.ota.tick(T + 2);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    f.ota.tick(T + 3);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA done"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_unknown_size_waits_for_connection() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(512, 'Y'), -2);
    MockHTTP::keepOpen = true;
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(512, (int)f.ota.bytesWritten());
    f.ota.tick(T + 2);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    MockHTTP::keepOpen = false;
    f.ota.tick(T + 3);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA done"));
}

void test_unknown_size_empty_aborts() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, "", -2);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    f.ota.tick(T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "Empty OTA"));
    TEST_ASSERT_TRUE(f.ota.pending());
}

void test_loads_check_request() {
    MockFS::put("/ota.pending", "check 1");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_TRUE(f.ota.slim());
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_EQUAL_STRING("", f.ota.availableVersion());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: resuming the check"));
    TEST_ASSERT_EQUAL_STRING("check 2", MockFS::files["/ota.pending"].c_str());
}

void test_recovery_check_installs() {
    MockFS::put("/ota.pending", "check 1");
    Fixture f;
    f.ota.begin();
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.tick(0);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_recovery_up_to_date_restores_panel() {
    MockFS::put("/ota.pending", "check 1");
    Fixture f;
    f.ota.begin();
    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.tick(0);
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "restoring the panel"));
}

void test_loads_pending_request() {
    MockFS::put("/ota.pending", "v129 1");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_TRUE(f.ota.slim());
    TEST_ASSERT_TRUE(f.ota.busy());
    TEST_ASSERT_EQUAL_STRING("v129", f.ota.availableVersion());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: resuming v129"));
    TEST_ASSERT_EQUAL_STRING("v129 2", MockFS::files["/ota.pending"].c_str());
}

void test_invalid_pending_request() {
    MockFS::put("/ota.pending", "junk 1");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "invalid recovery request"));
}

void test_blank_pending_request() {
    MockFS::put("/ota.pending", " \n");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "invalid recovery request"));
}

// A tag longer than 15 characters is cut there: not a version, so it goes away
void test_long_pending_request() {
    MockFS::put("/ota.pending", "v1234567890123456789 1");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "invalid recovery request"));
}

// Leading space and a newline at the end, as an editor would leave it
void test_pending_request_with_spaces() {
    MockFS::put("/ota.pending", " v42 2\n");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_TRUE(f.ota.slim());
    TEST_ASSERT_EQUAL_STRING("v42 3", MockFS::files["/ota.pending"].c_str());
}

// The tag ends at the newline: no counter, first attempt
void test_pending_request_ending_in_newline() {
    MockFS::put("/ota.pending", "v42\n");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_TRUE(f.ota.slim());
    TEST_ASSERT_EQUAL_STRING("v42 1", MockFS::files["/ota.pending"].c_str());
}

void test_pending_request_without_counter() {
    MockFS::put("/ota.pending", "v42");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_TRUE(f.ota.slim());
    TEST_ASSERT_EQUAL_STRING("v42", f.ota.availableVersion());
    TEST_ASSERT_EQUAL_STRING("v42 1", MockFS::files["/ota.pending"].c_str());
}

// Crashed (OOM) on the last allowed boot without reaching _persistAndReboot:
// the next boot must give up, not resume with the same counter.
void test_crash_spends_a_boot() {
    MockFS::put("/ota.pending", "v129 3");
    {
        Fixture crashed;
        crashed.ota.begin();
        TEST_ASSERT_TRUE(crashed.ota.slim());
    }
    logger = Logger();
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "recovery attempts exhausted"));
}

void test_rejected_image_is_not_downloaded_again() {
    Fixture f;
    Update._endOk = false;
    queueUpdate(std::string(100, 'X'));
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_TRUE(logHas(logger, "OTA error"));
    size_t calls = MockHTTP::calledURLs.size();

    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(2 * T + 1);
    TEST_ASSERT_EQUAL((int)calls + 1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.ota.pending());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: v999 was already rejected"));
    TEST_ASSERT_EQUAL_STRING("v999 rejected; waiting for another version", f.ota.detail());

    // Manual install is still possible
    Update._endOk = true;
    MockHTTP::push(200, std::string(64, 'X'));
    f.ota.requestInstall();
    f.ota.tick(2 * T + 2);
    TEST_ASSERT_TRUE(f.ota.inProgress());
}

void test_rejection_in_recovery_restores_panel() {
    MockFS::put("/ota.pending", "v129 1");
    Fixture f;
    f.ota.begin();
    Update._endOk = false;
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.tick(0);
    f.ota.tick(1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "recovery over, restoring the panel"));
    TEST_ASSERT_EQUAL_STRING("v129", MockFS::files["/ota.rejected"].c_str());
}

// Without heap for the panel the check goes into recovery: the mark must
// survive the restart, otherwise the device downloads the same image again.
void test_rejection_survives_restart() {
    MockFS::put("/ota.rejected", "v999");
    MockFS::put("/ota.pending", "check 1");
    Fixture f;
    f.ota.begin();
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(0);
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: v999 was already rejected"));
    TEST_ASSERT_TRUE(logHas(logger, "recovery done, restoring the panel"));
}

void test_new_version_ignores_old_rejection() {
    MockFS::put("/ota.rejected", "v998");
    Fixture f;
    f.ota.begin();
    queueUpdate(std::string(64, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
}

void test_rejection_without_fs_stays_in_ram() {
    Fixture f;
    Update._endOk = false;
    MockFS::failWriteOpen = true;
    queueUpdate(std::string(100, 'X'));
    f.ota.tick(T);
    f.ota.tick(T + 1);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: could not save the rejected version"));
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.tick(2 * T + 1);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: v999 was already rejected"));
}

void test_empty_pending_request() {
    MockFS::put("/ota.pending", "");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
}

void test_exhausted_pending_request() {
    MockFS::put("/ota.pending", "v129 4");
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "recovery attempts exhausted"));
}

void test_pending_request_does_not_open() {
    MockFS::put("/ota.pending", "v129 1");
    MockFS::failReadOpen = true;
    Fixture f;
    f.ota.begin();
    TEST_ASSERT_FALSE(f.ota.slim());
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
}

void test_runs_out_of_retries_and_saves_recovery() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    unsigned long t = 1;
    for (uint8_t i = 0; i < OTAService::MAX_TRIES; i++) {
        MockHTTP::push(404);
        if (i == 0) f.ota.requestInstall();
        f.ota.tick(t);
        t += OTAService::RETRY_MAX_MS;
    }
    TEST_ASSERT_TRUE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_TRUE(logHas(logger, "recovery on the next boot"));
}

void test_recovery_runs_out_and_restarts_panel() {
    MockFS::put("/ota.pending", "v129 3");
    Fixture f;
    f.ota.begin();
    unsigned long t = 1;
    for (uint8_t i = 0; i < OTAService::MAX_TRIES; i++) {
        MockHTTP::push(404);
        f.ota.tick(t);
        t += OTAService::RETRY_MAX_MS;
    }
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "gave up after recovery"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_FALSE(f.ota.slim());
}

void test_does_not_save_recovery_if_fs_fails() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    f.ota.requestCheck();
    f.ota.tick(0);
    MockFS::failWriteOpen = true;
    unsigned long t = 1;
    for (uint8_t i = 0; i < OTAService::MAX_TRIES; i++) {
        MockHTTP::push(404);
        if (i == 0) f.ota.requestInstall();
        f.ota.tick(t);
        t += OTAService::RETRY_MAX_MS;
    }
    TEST_ASSERT_TRUE(logHas(logger, "could not save the recovery request"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
}

void test_unknown_size_no_room() {
    Fixture f;
    ESP._freeSketch = 0;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(200, std::string(64, 'Z'), -2);
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "No room for OTA"));
}

void test_follows_absolute_redirect() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(302, "", -1, "https://cdn.example:443/fw.bin");
    MockHTTP::push(200, std::string(128, 'X'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_TRUE(logHas(logger, "OTA redirect 302"));
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "cdn.example"));
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "/fw.bin"));
}

void test_follows_relative_redirect() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(301, "", -1, "/rel/fw.bin");
    MockHTTP::push(200, std::string(64, 'Y'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "/rel/fw.bin"));
}

void test_invalid_redirect() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(307, "", -1, "http://cdn.example/fw.bin");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: invalid redirect"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_redirect_without_location() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(308);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: redirect without Location"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_too_many_redirects() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    for (int i = 0; i < 5; i++)
        MockHTTP::push(302, "", -1, "https://cdn.example/fw.bin");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: too many redirects"));
    TEST_ASSERT_FALSE(f.ota.inProgress());
}

void test_incomplete_headers() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\nContent-Length: 4";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: incomplete headers"));
}

void test_invalid_http_status() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "NOTHTTP 200\r\n\r\n";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: incomplete headers"));
}

void test_status_without_code() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "HTTP/1.0\r\n\r\n";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: incomplete headers"));
}

void test_cdn_uses_mfln() {
    Fixture f;
    MockTLS::mfln = true;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(302, "", -1, "https://cdn.example/fw.bin");
    MockHTTP::push(200, std::string(32, 'Z'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(4096, MockTLS::bufIn);
}

void test_redirect_without_path() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(303, "", -1, "https://cdn.example");
    MockHTTP::push(200, std::string(32, 'Q'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_TRUE(contains(MockHTTP::calledURLs.back(), "cdn.example/"));
}

void test_redirect_long_host() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    std::string loc = "https://" + std::string(80, 'a') + "/fw.bin";
    MockHTTP::push(302, "", -1, loc);
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: invalid redirect"));
}

void test_check_runs_out_without_saving_request() {
    Fixture f;
    f.ota.setAutomatic(false);
    unsigned long t = 0;
    f.ota.requestCheck();
    MockTLS::failConnect = true;
    for (uint8_t i = 0; i < OTAService::MAX_TRIES; i++) {
        f.ota.tick(t);
        t += OTAService::RETRY_MAX_MS;
        if (i + 1 < OTAService::MAX_TRIES && !f.ota.pending())
            f.ota.requestCheck();
    }
    TEST_ASSERT_FALSE(LittleFS.exists("/ota.pending"));
    TEST_ASSERT_TRUE(logHas(logger, "out of retries"));
}

// ── raw TLS/HTTP error paths ─────────────────────────────────────────────────

void test_check_incomplete_headers() {
    Fixture f;
    MockHTTP::rawAny = true;
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\nContent-Type: x";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Version JSON error: headers"));
    TEST_ASSERT_EQUAL_STRING("Retrying in 10 s", f.ota.detail());
}

// BearSSL allocates the buffers and the largest free block no longer fits the RX
void test_block_shrinks_after_tls_buffers() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockTLS::shrinkOnBuffers = 2; // 1 = check, 2 = download
    MockTLS::shrinkBlockTo = OTAService::GITHUB_RX + OTAService::TLS_TAIL - 1;
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: TLS unavailable"));
}

// Waiting for the response does not block: tick returns at once, the panel
// cannot stack requests and the deadline expires on a later tick.
void test_read_does_not_block_and_expires_without_data() {
    Fixture f;
    f.ota.setAutomatic(false);
    MockHTTP::push(200, R"({"name":"no tag")");
    MockHTTP::keepOpen = true;
    f.ota.requestCheck();
    unsigned long before = millis();
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(before, millis());           // no delay
    TEST_ASSERT_TRUE(f.ota.pending());
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_FALSE(logHas(logger, "JSON error"));

    f.ota.requestCheck();                          // session open: ignored
    TEST_ASSERT_EQUAL(OTAService::Report::Checking, f.ota.report());
    f.ota.tick(T + OTAService::READ_TIMEOUT - 1);
    TEST_ASSERT_FALSE(logHas(logger, "JSON error"));
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());

    f.ota.tick(T + OTAService::READ_TIMEOUT);
    TEST_ASSERT_TRUE(logHas(logger, "Version JSON error: tag_name"));
    TEST_ASSERT_EQUAL_STRING("Retrying in 10 s", f.ota.detail());
}

// Data arriving bit by bit: each tick consumes what came and goes on
void test_response_in_parts_across_ticks() {
    Fixture f;
    queueUpdate(std::string(64, 'X'));
    MockHTTP::keepOpen = true;
    f.ota.tick(T);                       // check and the download headers
    TEST_ASSERT_TRUE(f.ota.inProgress()); // the mock delivers everything at once

    setUp();
    Fixture g;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "HTTP/1.0 302 F\r\nLocation: /cdn\r\n";
    MockHTTP::keepOpen = true;           // incomplete redirect header
    g.ota.tick(T);
    TEST_ASSERT_FALSE(g.ota.inProgress());
    TEST_ASSERT_TRUE(g.ota.busy());
    g.ota.requestInstall();              // session open: ignored
    TEST_ASSERT_EQUAL(OTAService::Report::Available, g.ota.report());
    MockHTTP::keepOpen = false;          // the server closes without the empty line
    g.ota.tick(T + 1);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: incomplete headers"));
}

void test_header_without_colon_is_ignored() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = "HTTP/1.0 200 OK\r\nNoValue\r\nContent-Length: 4\r\n\r\nXXXX";
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(4, f.ota.contentLength());
}

void test_tag_with_trailing_spaces() {
    Fixture f;
    f.ota.setAutomatic(false);
    MockHTTP::push(200, "{\"tag_name\":\"v999\t \"}");
    f.ota.requestCheck();
    f.ota.tick(0);
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_download_without_memory_for_the_path() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    mock::failMallocSize = OTAService::OTA_PATH_CAP;
    f.ota.tick(T);
    TEST_ASSERT_FALSE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::calledURLs.size());
    TEST_ASSERT_TRUE(logHas(logger, "OTA: not enough memory"));
    TEST_ASSERT_TRUE(f.wifi.restartPending());
    TEST_ASSERT_EQUAL_STRING("v999 1", MockFS::files["/ota.pending"].c_str());
}

// ── download header parser ───────────────────────────────────────────────────

// A valid check and the raw response to the download's first GET
static void rawDownload(const std::string& http) {
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::rawHttp = http;
}

void test_cut_status() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: incomplete headers"));
}

void test_status_with_invalid_code() {
    const char* lines[] = {"HTTP/1.0 !00 x\r\n\r\n", "HTTP/1.0 x00 y\r\n\r\n"};
    for (const char* line : lines) {
        setUp();
        Fixture f;
        rawDownload(line);
        f.ota.tick(T);
        TEST_ASSERT_TRUE(logHas(logger, "OTA: incomplete headers"));
    }
}

void test_long_status_is_truncated() {
    Fixture f;
    rawDownload("HTTP/1.0 200 " + std::string(40, 'O') + "\r\nContent-Length: 4\r\n\r\nXXXX");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(4, f.ota.contentLength());
}

void test_header_cut_after_colon() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK\r\nX:");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: incomplete headers"));
}

void test_header_with_tab_after_colon() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK\r\nContent-Length:\t4\r\n\r\nXXXX");
    f.ota.tick(T);
    TEST_ASSERT_EQUAL(4, f.ota.contentLength());
}

void test_long_header_name() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK\r\n" + std::string(40, 'H') + ": v\r\nContent-Length: 4\r\n\r\nXXXX");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL(4, f.ota.contentLength());
}

void test_long_content_length_is_truncated() {
    Fixture f;
    rawDownload("HTTP/1.0 200 OK\r\nContent-Length: " + std::string(40, '0') + "\r\n\r\n");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "Invalid firmware size"));
}

void test_long_location_is_truncated() {
    Fixture f;
    rawDownload("HTTP/1.0 302 F\r\nLocation: /" + std::string(1300, 'a') + "\r\n\r\n");
    MockHTTP::push(200, std::string(32, 'Z'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    std::string expected = "https://github.com/" + std::string(OTAService::OTA_PATH_CAP - 2, 'a');
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), MockHTTP::calledURLs.back().c_str());
}

void test_first_location_wins() {
    Fixture f;
    rawDownload("HTTP/1.0 302 F\r\nLocation: /a\r\nLocation: /b\r\n\r\n");
    MockHTTP::push(200, std::string(32, 'Z'));
    f.ota.tick(T);
    TEST_ASSERT_TRUE(f.ota.inProgress());
    TEST_ASSERT_EQUAL_STRING("https://github.com/a", MockHTTP::calledURLs.back().c_str());
}

void test_redirect_without_host() {
    Fixture f;
    MockHTTP::push(200, R"({"tag_name":"v999"})");
    MockHTTP::push(302, "", -1, "https:///fw.bin");
    f.ota.tick(T);
    TEST_ASSERT_TRUE(logHas(logger, "OTA: invalid redirect"));
}

// ── tag_name parser ──────────────────────────────────────────────────────────

static void checkWith(Fixture& f, const std::string& body) {
    f.ota.setAutomatic(false);
    MockHTTP::push(200, body);
    f.ota.requestCheck();
    f.ota.tick(0);
}

void test_tag_after_repeated_quotes() {
    Fixture f;
    checkWith(f, R"({"a":"","tag_name":"v999"})");
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_tag_with_spaces_around_colon() {
    Fixture f;
    checkWith(f, "{\"tag_name\" \t\r\n: \t\r\n\"v999\"}");
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_tag_with_leading_tab() {
    Fixture f;
    checkWith(f, "{\"tag_name\":\"\t v999\"}");
    TEST_ASSERT_EQUAL_STRING("v999", f.ota.availableVersion());
}

void test_long_tag_is_truncated() {
    Fixture f;
    checkWith(f, R"({"tag_name":"v12345678901234567890"})");
    TEST_ASSERT_FALSE(logHas(logger, "Version JSON error"));
    TEST_ASSERT_EQUAL(OTAService::Report::UpToDate, f.ota.report()); // v12345678901234 is not a valid tag
}

void test_malformed_tag() {
    const char* bodies[] = {
        R"({"tag_name")",      // ends before the colon
        R"({"tag_name" x)",    // no colon
        R"({"tag_name":)",     // ends before the value
        R"({"tag_name":5})",   // the value is not a string
        R"({"tag_name":"v99)", // unclosed string
    };
    for (const char* body : bodies) {
        setUp();
        Fixture f;
        checkWith(f, body);
        TEST_ASSERT_TRUE_MESSAGE(logHas(logger, "Version JSON error: tag_name"), body);
        TEST_ASSERT_EQUAL_STRING("", f.ota.availableVersion());
    }
}

// ntfy releases TLS when OTA will check or download
void test_wants_net_follows_the_schedule() {
    Fixture f;
    TEST_ASSERT_FALSE(f.ota.wantsNet(OTAService::FIRST_CHECK - 1));
    TEST_ASSERT_TRUE(f.ota.wantsNet(OTAService::FIRST_CHECK));
    TEST_ASSERT_FALSE(f.ota.wantsNet(OTAService::DEADLINE));

    MockHTTP::push(200, R"({"tag_name":"dev"})");
    f.ota.tick(OTAService::FIRST_CHECK);
    TEST_ASSERT_FALSE(f.ota.wantsNet(OTAService::FIRST_CHECK + T - 1));
    TEST_ASSERT_TRUE(f.ota.wantsNet(OTAService::FIRST_CHECK + T));

    f.ota.setAutomatic(false);
    TEST_ASSERT_FALSE(f.ota.wantsNet(OTAService::FIRST_CHECK + T));
    f.ota.requestCheck();                              // request from the panel
    TEST_ASSERT_TRUE(f.ota.wantsNet(0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_tag);
    RUN_TEST(test_is_newer);
    RUN_TEST(test_tagged_hash_binds_the_version);
    RUN_TEST(test_no_check_before_interval);
    RUN_TEST(test_first_automatic_check);
    RUN_TEST(test_check_after_interval);
    RUN_TEST(test_no_check_after_deadline);
    RUN_TEST(test_no_check_with_restart_pending);
    RUN_TEST(test_no_check_without_wifi);
    RUN_TEST(test_api_connection_fails);
    RUN_TEST(test_api_http_error);
    RUN_TEST(test_api_invalid_json);
    RUN_TEST(test_empty_tag);
    RUN_TEST(test_starts_ota_with_the_build_file);
    RUN_TEST(test_file_matches_eboot);
    RUN_TEST(test_download_connection_fails);
    RUN_TEST(test_download_http_error);
    RUN_TEST(test_invalid_size);
    RUN_TEST(test_no_room);
    RUN_TEST(test_full_stream_schedules_restart);
    RUN_TEST(test_progress_logged_every_10_percent);
    RUN_TEST(test_invalid_signature_does_not_restart);
    RUN_TEST(test_write_error_aborts);
    RUN_TEST(test_wifi_drops_during_ota);
    RUN_TEST(test_stalled_stream_aborts_on_timeout);
    RUN_TEST(test_no_new_check_during_ota);
    RUN_TEST(test_automatic_off_does_not_check);
    RUN_TEST(test_manual_check_does_not_install);
    RUN_TEST(test_manual_check_already_up_to_date);
    RUN_TEST(test_manual_install);
    RUN_TEST(test_install_without_version_is_ignored);
    RUN_TEST(test_requests_ignored_during_download);
    RUN_TEST(test_wifi_drops_with_request_pending);
    RUN_TEST(test_check_not_enough_memory);
    RUN_TEST(test_check_block_too_small);
    RUN_TEST(test_check_tls_unavailable);
    RUN_TEST(test_download_not_enough_memory);
    RUN_TEST(test_download_tls_unavailable);
    RUN_TEST(test_success_deletes_request);
    RUN_TEST(test_github_uses_large_tls_buffer);
    RUN_TEST(test_retries_after_backoff);
    RUN_TEST(test_unknown_size_completes_at_end_of_stream);
    RUN_TEST(test_unknown_size_waits_for_connection);
    RUN_TEST(test_unknown_size_empty_aborts);
    RUN_TEST(test_loads_check_request);
    RUN_TEST(test_recovery_check_installs);
    RUN_TEST(test_recovery_up_to_date_restores_panel);
    RUN_TEST(test_loads_pending_request);
    RUN_TEST(test_invalid_pending_request);
    RUN_TEST(test_blank_pending_request);
    RUN_TEST(test_long_pending_request);
    RUN_TEST(test_pending_request_with_spaces);
    RUN_TEST(test_pending_request_ending_in_newline);
    RUN_TEST(test_pending_request_without_counter);
    RUN_TEST(test_crash_spends_a_boot);
    RUN_TEST(test_rejected_image_is_not_downloaded_again);
    RUN_TEST(test_rejection_in_recovery_restores_panel);
    RUN_TEST(test_rejection_survives_restart);
    RUN_TEST(test_new_version_ignores_old_rejection);
    RUN_TEST(test_rejection_without_fs_stays_in_ram);
    RUN_TEST(test_empty_pending_request);
    RUN_TEST(test_exhausted_pending_request);
    RUN_TEST(test_pending_request_does_not_open);
    RUN_TEST(test_runs_out_of_retries_and_saves_recovery);
    RUN_TEST(test_recovery_runs_out_and_restarts_panel);
    RUN_TEST(test_does_not_save_recovery_if_fs_fails);
    RUN_TEST(test_unknown_size_no_room);
    RUN_TEST(test_check_runs_out_without_saving_request);
    RUN_TEST(test_follows_absolute_redirect);
    RUN_TEST(test_follows_relative_redirect);
    RUN_TEST(test_invalid_redirect);
    RUN_TEST(test_redirect_without_location);
    RUN_TEST(test_too_many_redirects);
    RUN_TEST(test_incomplete_headers);
    RUN_TEST(test_invalid_http_status);
    RUN_TEST(test_status_without_code);
    RUN_TEST(test_cdn_uses_mfln);
    RUN_TEST(test_redirect_without_path);
    RUN_TEST(test_redirect_long_host);
    RUN_TEST(test_check_incomplete_headers);
    RUN_TEST(test_block_shrinks_after_tls_buffers);
    RUN_TEST(test_read_does_not_block_and_expires_without_data);
    RUN_TEST(test_response_in_parts_across_ticks);
    RUN_TEST(test_header_without_colon_is_ignored);
    RUN_TEST(test_tag_with_trailing_spaces);
    RUN_TEST(test_download_without_memory_for_the_path);
    RUN_TEST(test_cut_status);
    RUN_TEST(test_status_with_invalid_code);
    RUN_TEST(test_long_status_is_truncated);
    RUN_TEST(test_header_cut_after_colon);
    RUN_TEST(test_header_with_tab_after_colon);
    RUN_TEST(test_long_header_name);
    RUN_TEST(test_long_content_length_is_truncated);
    RUN_TEST(test_long_location_is_truncated);
    RUN_TEST(test_first_location_wins);
    RUN_TEST(test_redirect_without_host);
    RUN_TEST(test_tag_after_repeated_quotes);
    RUN_TEST(test_tag_with_spaces_around_colon);
    RUN_TEST(test_tag_with_leading_tab);
    RUN_TEST(test_long_tag_is_truncated);
    RUN_TEST(test_malformed_tag);
    RUN_TEST(test_wants_net_follows_the_schedule);
    return UNITY_END();
}
