// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Any Cloudflare API response: to the record PATCH, to the zone list and
// to the record lookup (the streaming reader, CloudflareApi.h). The first
// byte picks the HTTP code.
#include "../test_support.h"
#include "Clock.h"
#include "Config.h"
#include "WiFiService.h"
#include "CloudflareSetup.h"

static time_t fakeTime() { return Clock::MIN_VALID_EPOCH + 100; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    resetMocks();
    Logger log;
    Config cfg;
    strlcpy(cfg.cf_token, "TOKEN", sizeof(cfg.cf_token));
    strlcpy(cfg.cf_zone, "ZONE", sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_record, "REC0", sizeof(cfg.cf_record));
    strlcpy(cfg.publicIP, "1.2.3.4", sizeof(cfg.publicIP));

    Clock         clock{fakeTime};
    WiFiService   wifi{cfg, log};
    CloudflareDNS dns{cfg, log, wifi, clock};

    static const int CODES[] = {200, 400, 403, 500};
    const std::string body(reinterpret_cast<const char*>(data) + 1, size - 1);
    MockHTTP::push(CODES[data[0] % 4], body);
    dns.tick(0);

    ConfigStore     store{cfg, log};
    CloudflareSetup cf{cfg, log, store, clock, &dns};
    for (int i = 0; i < 2; i++) MockHTTP::push(CODES[data[0] % 4], body);
    cf.scan();
    cf.tick(0);
    MockFS::put(CloudflareSetup::ZONES_PATH,
                "023e105f4ecef8ad9ca31a8372d0c353 01a7362d577a6c3019a474fd6f485823 example.com\n");
    for (int i = 0; i < 2; i++) MockHTTP::push(CODES[data[0] % 4], body);
    cf.record("023e105f4ecef8ad9ca31a8372d0c353", "home.example.com");
    cf.tick(0);
    return 0;
}
