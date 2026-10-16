// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include <vector>
#include "../test_support.h"
#include "Config.h"
#include "Clock.h"
#include "WiFiService.h"
#include "CloudflareSetup.h"

static time_t fakeNow;
static time_t fakeTime() { return fakeNow; }

static Logger logger;
static Config cfg;

static const char* Z1 = "023e105f4ecef8ad9ca31a8372d0c353";
static const char* Z2 = "1111105f4ecef8ad9ca31a8372d0c353";
static const char* A1 = "01a7362d577a6c3019a474fd6f485823";
static const char* A2 = "22a7362d577a6c3019a474fd6f485823";
static const char* R1 = "372e67954025e0ba6aaa6d586b9e0b59";

void setUp() {
    resetMocks();
    logger = Logger();
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    cfg = Config{};
    strlcpy(cfg.cf_token, "TOKEN", sizeof(cfg.cf_token));
}
void tearDown() {}

struct Fixture {
    Clock           clock{fakeTime};
    ConfigStore     store{cfg, logger};
    WiFiService     wifi{cfg, logger};
    CloudflareDNS   dns{cfg, logger, wifi, clock};
    CloudflareSetup cf{cfg, logger, store, clock, &dns};
};

// A zone as the API lists it: account, owner and plan carry IDs too
static std::string zone(const char* id, const char* name, const char* account) {
    return std::string(R"({"id":")") + id + R"(","name":")" + name +
           R"(","status":"active","paused":false,"name_servers":["a.ns.cloudflare.com","b.ns.cloudflare.com"],)"
           R"("owner":{"id":"99999999999999999999999999999999","type":"user"},)"
           R"("account":{"id":")" + account + R"(","name":"Me","tag":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},)"
           R"("plan":{"id":"0feeeeeeeeeeeeeeeeeeeeeeeeeeeeee","name":"Free Website"},"meta":{"step":2}})";
}

static std::string list(const std::string& items) {
    return R"({"result":[)" + items + R"(],"result_info":{"page":1,"count":2},"total":2,"success":true,"errors":[],"messages":[]})";
}

static std::string zoneFile() { return MockFS::files[CloudflareSetup::ZONES_PATH]; }

// ── scan ─────────────────────────────────────────────────────────────────────

void test_scan_lists_the_zones() {
    Fixture f;
    TEST_ASSERT_FALSE(f.cf.due());
    TEST_ASSERT_EQUAL(CloudflareSetup::IDLE, f.cf.state());
    f.cf.scan();
    TEST_ASSERT_TRUE(f.cf.due());
    TEST_ASSERT_EQUAL(CloudflareSetup::WORKING, f.cf.state());
    MockHTTP::push(200, list(zone(Z1, "example.com", A1) + "," + zone(Z2, "other.org", A2)));
    f.cf.tick(0);
    TEST_ASSERT_FALSE(f.cf.due());
    TEST_ASSERT_EQUAL(CloudflareSetup::DONE, f.cf.state());
    TEST_ASSERT_EQUAL(CloudflareSetup::SCAN, f.cf.last());
    TEST_ASSERT_EQUAL(2, f.cf.zones());
    TEST_ASSERT_EQUAL_STRING((std::string(Z1) + " " + A1 + " example.com\n" + Z2 + " " + A2 + " other.org\n").c_str(),
                             zoneFile().c_str());
    TEST_ASSERT_EQUAL_STRING(A1, cfg.cf_account);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], A1));
    TEST_ASSERT_TRUE(logHas(logger, "Cloudflare: token reaches 2 zone(s)"));

    const MockHTTP::Request& r = MockHTTP::requests[0];
    TEST_ASSERT_EQUAL_STRING("https://api.cloudflare.com/client/v4/zones?per_page=20&status=active", r.url.c_str());
    TEST_ASSERT_EQUAL_STRING("GET", r.method.c_str());
    TEST_ASSERT_EQUAL_STRING("Bearer TOKEN", r.header("Authorization").c_str());
    TEST_ASSERT_EQUAL_STRING("application/json", r.header("Content-Type").c_str());
    TEST_ASSERT_TRUE(cloudflareTrust(r.trust));
    TEST_ASSERT_EQUAL((long)fakeNow, r.x509Time);

    CloudflareSetup::Zone z;
    TEST_ASSERT_TRUE(CloudflareSetup::find(Z2, z));
    TEST_ASSERT_EQUAL_STRING("other.org", z.name);
    TEST_ASSERT_EQUAL_STRING(A2, z.account);
    TEST_ASSERT_FALSE(CloudflareSetup::find(R1, z));
}

// Only whole zones with a 32-digit ID and a name that fits; at most 20
void test_scan_skips_odd_zones_and_caps_the_list() {
    Fixture f;
    std::string items = zone("not-an-id", "bad.com", A1) + "," +
                        zone("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", "notHex.com", A1) + "," +
                        R"({"id":")" + std::string(Z1) + R"(","name":")" + std::string(80, 'n') + R"("})" + "," +
                        R"({"name":"noid.com"})";
    for (int i = 0; i < 25; i++) {
        char id[33];
        snprintf(id, sizeof(id), "%032x", i + 1);
        items += "," + zone(id, ("z" + std::to_string(i) + ".com").c_str(), A1);
    }
    f.cf.scan();
    MockHTTP::push(200, list(items));
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(cfapi::MAX_ZONES, f.cf.zones());
    TEST_ASSERT_FALSE(contains(zoneFile(), "bad.com"));
    TEST_ASSERT_FALSE(contains(zoneFile(), "notHex.com"));
    TEST_ASSERT_FALSE(contains(zoneFile(), "noid.com"));
    TEST_ASSERT_FALSE(contains(zoneFile(), Z1));
    TEST_ASSERT_TRUE(contains(zoneFile(), " z19.com\n"));
    TEST_ASSERT_FALSE(contains(zoneFile(), "z20.com"));
}

// A token with no zone (workers.dev only): the account comes from the account list
void test_scan_without_zones_reads_the_account() {
    Fixture f;
    f.cf.scan();
    MockHTTP::push(200, list(""));
    MockHTTP::push(200, list(std::string(R"({"id":")") + A2 + R"(","name":"Me","type":"standard"})"));
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::DONE, f.cf.state());
    TEST_ASSERT_EQUAL(0, f.cf.zones());
    TEST_ASSERT_EQUAL_STRING(A2, cfg.cf_account);
    TEST_ASSERT_EQUAL_STRING("https://api.cloudflare.com/client/v4/accounts?per_page=1",
                             MockHTTP::requests[1].url.c_str());
}

void test_scan_failures() {
    Fixture f;
    // The API's own message
    f.cf.scan();
    MockHTTP::push(403, R"({"success":false,"errors":[{"code":9109,"message":"Invalid access token"},{"message":"second"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::FAILED, f.cf.state());
    TEST_ASSERT_EQUAL_STRING("Invalid access token", f.cf.message());
    TEST_ASSERT_TRUE(logHas(logger, "Cloudflare: Invalid access token"));

    // No message: the HTTP status
    f.cf.scan();
    TEST_ASSERT_EQUAL_STRING("", f.cf.message());
    MockHTTP::push(502, "<html>Bad gateway</html>");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("HTTP 502", f.cf.message());

    // The account list fails
    f.cf.scan();
    MockHTTP::push(200, list(""));
    MockHTTP::push(403, R"({"success":false,"errors":[{"message":"no account access"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("no account access", f.cf.message());

    // No connection
    f.cf.scan();
    MockHTTP::failNextBegin();
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("no connection (-1)", f.cf.message());

    // The list cannot be written
    f.cf.scan();
    MockFS::failWriteOpen = true;
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("cannot write the zone list", f.cf.message());
    MockFS::failWriteOpen = false;

    // Memory and TLS
    f.cf.scan();
    ESP._heap = PublicIPResolver::MIN_TLS_HEAP - 1;
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("not enough memory", f.cf.message());
    ESP._heap = 40000;
    f.cf.scan();
    MockTLS::nullOwned = true;
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("TLS unavailable", f.cf.message());
}

// Waits for Wi-Fi and a valid clock (the certificate)
void test_due_needs_wifi_and_clock() {
    Fixture f;
    f.cf.scan();
    WiFi._status = WL_DISCONNECTED;
    TEST_ASSERT_FALSE(f.cf.due());
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::calledURLs.size());
    WiFi._status = WL_CONNECTED;
    fakeNow = 0;
    TEST_ASSERT_FALSE(f.cf.due());
    fakeNow = Clock::MIN_VALID_EPOCH + 100;
    TEST_ASSERT_TRUE(f.cf.due());
}

// ── record ───────────────────────────────────────────────────────────────────

static void zonesOnDisk() {
    MockFS::put(CloudflareSetup::ZONES_PATH,
                std::string(Z1) + " " + A1 + " example.com\n" + Z2 + " " + A2 + " other.org\n");
}

void test_record_found_by_name() {
    zonesOnDisk();
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));
    strlcpy(cfg.cf_zone, Z1, sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_record, "old", sizeof(cfg.cf_record));
    Fixture f;
    MockHTTP::push(200, R"({"success":true,"errors":[],"result":{}})");
    f.dns.tick(0);                                         // synced once already
    TEST_ASSERT_FALSE(f.dns.due(1));

    f.cf.record(Z2, "home.other.org");
    MockHTTP::push(200, list(std::string(R"({"id":")") + R1 + R"(","zone_id":")" + Z2 +
                             R"(","name":"home.other.org","type":"A","content":"9.9.9.9"})"));
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::DONE, f.cf.state());
    TEST_ASSERT_EQUAL(CloudflareSetup::RECORD, f.cf.last());
    TEST_ASSERT_EQUAL_STRING(
        (std::string("https://api.cloudflare.com/client/v4/zones/") + Z2 + "/dns_records?type=A&name=home.other.org").c_str(),
        MockHTTP::requests.back().url.c_str());
    TEST_ASSERT_EQUAL_STRING(Z2, cfg.cf_zone);
    TEST_ASSERT_EQUAL_STRING(A2, cfg.cf_account);
    TEST_ASSERT_EQUAL_STRING(R1, cfg.cf_record);
    TEST_ASSERT_EQUAL_STRING("home.other.org", cfg.cf_host);
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], R1));
    TEST_ASSERT_TRUE(f.dns.due(1));                        // the new record gets the IP now
}

void test_record_created_when_missing() {
    zonesOnDisk();
    Fixture f;
    f.cf.record(Z1, "example.com");
    MockHTTP::push(200, list(""));
    MockHTTP::push(200, std::string(R"({"result":{"id":")") + R1 + R"(","name":"example.com"},"success":true})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::DONE, f.cf.state());
    const MockHTTP::Request& post = MockHTTP::requests[1];
    TEST_ASSERT_EQUAL_STRING("POST", post.method.c_str());
    TEST_ASSERT_EQUAL_STRING((std::string("https://api.cloudflare.com/client/v4/zones/") + Z1 + "/dns_records").c_str(),
                             post.url.c_str());
    TEST_ASSERT_EQUAL_STRING(R"({"type":"A","name":"example.com","content":"192.0.2.1","ttl":1,"proxied":false})",
                             post.payload.c_str());
    TEST_ASSERT_EQUAL_STRING(R1, cfg.cf_record);
    TEST_ASSERT_TRUE(logHas(logger, "Cloudflare: record example.com created"));

    // Without a DNS service to tell (it is optional)
    ConfigStore store{cfg, logger};
    CloudflareSetup bare{cfg, logger, store, f.clock};
    bare.record(Z2, "c.other.org");
    MockHTTP::push(200, list(std::string(R"({"id":")") + R1 + R"("})"));
    bare.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::DONE, bare.state());

    // With the public IP known, the record starts there
    strlcpy(cfg.publicIP, "5.6.7.8", sizeof(cfg.publicIP));
    f.cf.record(Z1, "b.example.com");
    MockHTTP::push(200, list(""));
    MockHTTP::push(200, std::string(R"({"result":{"id":")") + R1 + R"("},"success":true})");
    f.cf.tick(0);
    TEST_ASSERT_TRUE(contains(MockHTTP::requests.back().payload, "\"content\":\"5.6.7.8\""));
}

void test_record_failures() {
    zonesOnDisk();
    Fixture f;
    f.cf.record(R1, "x.example.com");                     // not in the list
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::FAILED, f.cf.state());
    TEST_ASSERT_EQUAL_STRING("unknown zone", f.cf.message());
    TEST_ASSERT_EQUAL(0, (int)MockHTTP::requests.size());

    f.cf.record(Z1, "x.example.com");
    MockHTTP::push(403, R"({"success":false,"errors":[{"message":"dns read denied"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("dns read denied", f.cf.message());

    f.cf.record(Z1, "x.example.com");
    MockHTTP::push(200, list(""));
    MockHTTP::push(403, R"({"success":false,"errors":[{"message":"dns write denied"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("dns write denied", f.cf.message());
    TEST_ASSERT_EQUAL_STRING("", cfg.cf_record);
}

// ── the zone list file ───────────────────────────────────────────────────────

void test_zone_list_lines() {
    MockFS::put(CloudflareSetup::ZONES_PATH,
                "short line\n" + std::string(Z1) + "x" + A1 + " bad-separator.com\n" +
                std::string(Z1) + " " + A1 + "xbad-separator.com\n" + std::string(Z1) + " " + A1 + " " +
                std::string(200, 'n') + "\n\n" + Z2 + " " + A2 + " last.org");
    File f = LittleFS.open(CloudflareSetup::ZONES_PATH, "r");
    CloudflareSetup::Zone z;
    TEST_ASSERT_TRUE(CloudflareSetup::next(f, z));          // the long name is cut to fit
    TEST_ASSERT_EQUAL_STRING(Z1, z.id);
    TEST_ASSERT_EQUAL((int)sizeof(z.name) - 1, (int)strlen(z.name));
    TEST_ASSERT_TRUE(CloudflareSetup::next(f, z));          // no newline at the end
    TEST_ASSERT_EQUAL_STRING("last.org", z.name);
    TEST_ASSERT_FALSE(CloudflareSetup::next(f, z));
    f.close();

    MockFS::reset();                                        // no list at all
    TEST_ASSERT_FALSE(CloudflareSetup::find(Z1, z));
}

// ── deploy ───────────────────────────────────────────────────────────────────

static const char* NOT_FOUND = R"({"success":false,"errors":[{"code":10007,"message":"workers.api.error.script_not_found"}]})";
static const char* OK = R"({"success":true,"errors":[],"result":{}})";
static const char* OURS = R"({"success":true,"result":{"bindings":[{"type":"secret_text","name":"SECRET"},)"
                          R"({"type":"durable_object_namespace","name":"DEVICE","class_name":"Device"},)"
                          R"({"type":"durable_object_namespace","name":"OTHER","class_name":"Other"}]}})";

// The script as the firmware expands it, by the same rule as the tool
static std::string expanded() {
    std::string out;
    for (size_t i = 0; i < sizeof(RELAY_SCRIPT); i++) {
        std::vector<uint8_t> stack{RELAY_SCRIPT[i]};
        while (!stack.empty()) {
            uint8_t b = stack.back();
            stack.pop_back();
            int r = -1;
            for (int i = 0x7F, k = 0; k < 160; k++, i = i == 0xFF ? 0 : i + 1 + (i == 9)) {
                if (i == b) r = k;
            }
            if (r < 0) {
                out += (char)b;
            } else {
                stack.push_back(RELAY_PAIRS[2 * r + 1]);
                stack.push_back(RELAY_PAIRS[2 * r]);
            }
        }
    }
    return out;
}

static const MockHTTP::Request* find(const char* method, const char* urlPart) {
    for (auto& r : MockHTTP::requests)
        if (r.method == method && contains(r.url, urlPart)) return &r;
    return nullptr;
}

void test_deploy_on_own_domain() {
    zonesOnDisk();
    Fixture f;
    f.cf.deploy("esp-pump", Z1, "");
    TEST_ASSERT_TRUE(f.cf.due());
    MockHTTP::push(404, NOT_FOUND);
    MockHTTP::push(200, OK);
    MockHTTP::push(200, OK);
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("", f.cf.message());
    TEST_ASSERT_EQUAL(CloudflareSetup::DONE, f.cf.state());
    TEST_ASSERT_EQUAL(CloudflareSetup::DEPLOY, f.cf.last());
    TEST_ASSERT_EQUAL_STRING("esp-pump.example.com", cfg.relay_host);
    TEST_ASSERT_EQUAL_STRING("esp-pump", cfg.relay_name);
    TEST_ASSERT_EQUAL_STRING(Z1, cfg.relay_zone);
    TEST_ASSERT_EQUAL(RELAY_VERSION, cfg.relay_ver);
    TEST_ASSERT_EQUAL(64, (int)strlen(cfg.relay_secret));
    TEST_ASSERT_EQUAL(64, (int)strspn(cfg.relay_secret, "0123456789abcdef"));
    TEST_ASSERT_TRUE(contains(MockFS::files["/config.json"], cfg.relay_secret));
    TEST_ASSERT_TRUE(f.cf.takeDeployed());
    TEST_ASSERT_FALSE(f.cf.takeDeployed());
    TEST_ASSERT_TRUE(logHas(logger, "Cloudflare: relay at esp-pump.example.com"));

    std::string acc = std::string("/client/v4/accounts/") + A1 + "/workers/";
    TEST_ASSERT_NOT_NULL(find("GET", (acc + "scripts/esp-pump/settings").c_str()));

    // The upload: metadata with the secret and the first migration, then the script
    const MockHTTP::Request* up = find("PUT", (acc + "scripts/esp-pump").c_str());
    TEST_ASSERT_NOT_NULL(up);
    TEST_ASSERT_TRUE(contains(up->header("Content-Type"), "multipart/form-data; boundary="));
    TEST_ASSERT_EQUAL_STRING(std::to_string(up->payload.size()).c_str(), up->header("Content-Length").c_str());
    TEST_ASSERT_TRUE(contains(up->payload, "name=\"metadata\""));
    TEST_ASSERT_TRUE(contains(up->payload, (std::string("{\"type\":\"secret_text\",\"name\":\"SECRET\",\"text\":\"") +
                                            cfg.relay_secret + "\"}").c_str()));
    TEST_ASSERT_TRUE(contains(up->payload, "{\"type\":\"durable_object_namespace\",\"name\":\"DEVICE\",\"class_name\":\"Device\"}"));
    TEST_ASSERT_TRUE(contains(up->payload, "\"migrations\":{\"new_tag\":\"v1\",\"new_sqlite_classes\":[\"Device\"]}"));
    TEST_ASSERT_TRUE(contains(up->payload, "\"main_module\":\"worker.mjs\""));
    std::string script = expanded();
    TEST_ASSERT_EQUAL((int)RELAY_SCRIPT_LEN, (int)script.size());
    TEST_ASSERT_TRUE(contains(up->payload, ("Content-Type: application/javascript+module\r\n\r\n" + script).c_str()));
    TEST_ASSERT_TRUE(script.rfind("const VERSION = 4;\n", 0) == 0);
    TEST_ASSERT_TRUE(contains(script, "export class Device"));

    // The address on the domain
    const MockHTTP::Request* dom = find("PUT", (acc + "domains").c_str());
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQUAL_STRING((std::string(R"({"hostname":"esp-pump.example.com","service":"esp-pump","zone_id":")") + Z1 + "\"}").c_str(),
                             dom->payload.c_str());
}

// Already a relay: uploaded again, without the migration, with a new secret
void test_deploy_again_over_its_own_worker() {
    zonesOnDisk();
    strlcpy(cfg.relay_secret, "old", sizeof(cfg.relay_secret));
    Fixture f;
    f.cf.deploy("esp-pump", Z1, "");
    MockHTTP::push(200, OURS);
    MockHTTP::push(200, OK);
    MockHTTP::push(200, OK);
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::DONE, f.cf.state());
    TEST_ASSERT_FALSE(contains(MockHTTP::requests[1].payload, "migrations"));
    TEST_ASSERT_TRUE(strcmp(cfg.relay_secret, "old") != 0);
}

void test_deploy_on_workers_dev() {
    strlcpy(cfg.cf_account, A2, sizeof(cfg.cf_account));
    Fixture f;
    f.cf.deploy("esp-light", "", "");
    MockHTTP::push(404, NOT_FOUND);
    MockHTTP::push(200, R"({"result":{"subdomain":"myacc"},"success":true})");
    MockHTTP::push(200, OK);
    MockHTTP::push(200, OK);
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("", f.cf.message());
    TEST_ASSERT_EQUAL_STRING("esp-light.myacc.workers.dev", cfg.relay_host);
    TEST_ASSERT_EQUAL_STRING("", cfg.relay_zone);
    std::string acc = std::string("/client/v4/accounts/") + A2 + "/workers/";
    TEST_ASSERT_NOT_NULL(find("GET", (acc + "subdomain").c_str()));
    const MockHTTP::Request* on = find("POST", (acc + "scripts/esp-light/subdomain").c_str());
    TEST_ASSERT_NOT_NULL(on);
    TEST_ASSERT_EQUAL_STRING(R"({"enabled":true,"previews_enabled":false})", on->payload.c_str());
}

// An account without a workers.dev name gets the one from the panel
void test_deploy_names_the_workers_dev_subdomain() {
    strlcpy(cfg.cf_account, A2, sizeof(cfg.cf_account));
    Fixture f;
    f.cf.deploy("esp-light", "", "");
    MockHTTP::push(404, NOT_FOUND);
    MockHTTP::push(404, R"({"success":false,"errors":[{"code":10007,"message":"no subdomain"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::FAILED, f.cf.state());
    TEST_ASSERT_EQUAL_STRING("the account needs a workers.dev name", f.cf.message());

    f.cf.deploy("esp-light", "", "newacc");
    MockHTTP::push(404, NOT_FOUND);
    MockHTTP::push(200, R"({"result":{"subdomain":""},"success":true})");     // none yet
    MockHTTP::push(200, R"({"result":{"subdomain":"newacc"},"success":true})");
    MockHTTP::push(200, OK);
    MockHTTP::push(200, OK);
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("esp-light.newacc.workers.dev", cfg.relay_host);
    const MockHTTP::Request* put = find("PUT", (std::string("/client/v4/accounts/") + A2 + "/workers/subdomain").c_str());
    TEST_ASSERT_NOT_NULL(put);
    TEST_ASSERT_EQUAL_STRING(R"({"subdomain":"newacc"})", put->payload.c_str());

    // The name is taken
    cfg.relay_host[0] = '\0';
    f.cf.deploy("esp-light", "", "taken");
    MockHTTP::push(404, NOT_FOUND);
    MockHTTP::push(404, R"({"success":false,"errors":[{"message":"no subdomain"}]})");
    MockHTTP::push(409, R"({"success":false,"errors":[{"message":"subdomain unavailable"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("subdomain unavailable", f.cf.message());
    TEST_ASSERT_EQUAL_STRING("", cfg.relay_host);
}

void test_deploy_refusals() {
    zonesOnDisk();
    Fixture f;
    // Another Worker already has the name: left alone
    f.cf.deploy("site", Z1, "");
    MockHTTP::push(200, R"({"success":true,"result":{"bindings":[{"type":"kv_namespace","name":"KV","class_name":"Other"}]}})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("another Worker has this name", f.cf.message());
    TEST_ASSERT_EQUAL(1, (int)MockHTTP::requests.size());

    // The check itself fails (not a missing Worker)
    f.cf.deploy("esp-pump", Z1, "");
    MockHTTP::push(403, R"({"success":false,"errors":[{"message":"no workers access"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("no workers access", f.cf.message());

    f.cf.deploy("esp-pump", R1, "");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("unknown zone", f.cf.message());

    f.cf.deploy("esp-pump", "", "");                      // workers.dev without an account
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("no account: save and check the token", f.cf.message());

    // An address longer than the config holds
    MockFS::put(CloudflareSetup::ZONES_PATH, std::string(Z1) + " " + A1 + " " + std::string(60, 'd') + ".com\n");
    f.cf.deploy("esp-pump-with-a-long-name", Z1, "");
    MockHTTP::push(404, NOT_FOUND);
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("address too long", f.cf.message());
    TEST_ASSERT_EQUAL_STRING("", cfg.relay_host);
}

// A failed step leaves the relay as it was, secret included
void test_deploy_failing_steps_change_nothing() {
    zonesOnDisk();
    strlcpy(cfg.relay_secret, "keep", sizeof(cfg.relay_secret));
    Fixture f;
    f.cf.deploy("esp-pump", Z1, "");
    MockHTTP::push(404, NOT_FOUND);
    MockHTTP::push(400, R"({"success":false,"errors":[{"message":"script too large"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("script too large", f.cf.message());
    TEST_ASSERT_EQUAL_STRING("keep", cfg.relay_secret);

    f.cf.deploy("esp-pump", Z1, "");
    MockHTTP::push(404, NOT_FOUND);
    MockHTTP::push(200, OK);
    MockHTTP::push(409, R"({"success":false,"errors":[{"message":"hostname has DNS records"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("hostname has DNS records", f.cf.message());
    TEST_ASSERT_EQUAL_STRING("", cfg.relay_host);

    strlcpy(cfg.cf_account, A2, sizeof(cfg.cf_account));
    f.cf.deploy("esp-pump", "", "");
    MockHTTP::push(404, NOT_FOUND);
    MockHTTP::push(200, R"({"result":{"subdomain":"myacc"},"success":true})");
    MockHTTP::push(200, OK);
    MockHTTP::push(500, R"({"success":false,"errors":[{"message":"enable failed"}]})");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL_STRING("enable failed", f.cf.message());
    TEST_ASSERT_FALSE(f.cf.takeDeployed());
}

// After an update with another relay protocol, the Worker goes up again
void test_redeploy_after_a_protocol_change() {
    zonesOnDisk();
    Fixture f;
    f.cf.redeploy(0);                                     // no relay: nothing
    TEST_ASSERT_FALSE(f.cf.due());
    strlcpy(cfg.relay_host, "esp-pump.example.com", sizeof(cfg.relay_host));
    strlcpy(cfg.relay_name, "esp-pump", sizeof(cfg.relay_name));
    strlcpy(cfg.relay_zone, Z1, sizeof(cfg.relay_zone));
    cfg.relay_ver = RELAY_VERSION;
    f.cf.redeploy(0);                                     // current: nothing
    TEST_ASSERT_FALSE(f.cf.due());

    cfg.relay_ver = RELAY_VERSION - 1;
    f.cf.redeploy(0);
    TEST_ASSERT_TRUE(f.cf.due());
    f.cf.redeploy(1);                                     // busy: nothing new
    MockHTTP::push(500, "");
    f.cf.tick(0);
    TEST_ASSERT_EQUAL(CloudflareSetup::FAILED, f.cf.state());
    TEST_ASSERT_TRUE(contains(MockHTTP::requests[0].url, "/scripts/esp-pump/settings"));

    f.cf.redeploy(CloudflareSetup::REDEPLOY_MS - 1);      // at most once an hour
    TEST_ASSERT_FALSE(f.cf.due());
    f.cf.redeploy(CloudflareSetup::REDEPLOY_MS);
    TEST_ASSERT_TRUE(f.cf.due());
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_scan_lists_the_zones);
    RUN_TEST(test_scan_skips_odd_zones_and_caps_the_list);
    RUN_TEST(test_scan_without_zones_reads_the_account);
    RUN_TEST(test_scan_failures);
    RUN_TEST(test_due_needs_wifi_and_clock);
    RUN_TEST(test_record_found_by_name);
    RUN_TEST(test_record_created_when_missing);
    RUN_TEST(test_record_failures);
    RUN_TEST(test_zone_list_lines);
    RUN_TEST(test_deploy_on_own_domain);
    RUN_TEST(test_deploy_again_over_its_own_worker);
    RUN_TEST(test_deploy_on_workers_dev);
    RUN_TEST(test_deploy_names_the_workers_dev_subdomain);
    RUN_TEST(test_deploy_refusals);
    RUN_TEST(test_deploy_failing_steps_change_nothing);
    RUN_TEST(test_redeploy_after_a_protocol_change);
    return UNITY_END();
}
