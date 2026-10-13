// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Resposta qualquer da API do Cloudflare ao PATCH do registro. O primeiro
// byte escolhe o código HTTP.
#include "../test_support.h"
#include "Clock.h"
#include "Config.h"
#include "WiFiService.h"
#include "PublicIPResolver.h"
#include "CloudflareDNS.h"

static time_t fakeTime() { return Clock::MIN_VALID_EPOCH + 100; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    resetMocks();
    Logger log;
    Config cfg;
    strlcpy(cfg.cf_token, "TOKEN", sizeof(cfg.cf_token));
    strlcpy(cfg.cf_zone, "ZONE", sizeof(cfg.cf_zone));
    strlcpy(cfg.cf_records[0], "REC0", sizeof(cfg.cf_records[0]));
    cfg.cf_record_count = 1;

    Clock            clock{fakeTime};
    WiFiService      wifi{cfg, log};
    PublicIPResolver resolver{log, clock};
    CloudflareDNS    dns{cfg, log, wifi, resolver, clock};

    static const int CODES[] = {200, 400, 403, 500};
    MockHTTP::push(200, "1.2.3.4");
    MockHTTP::push(CODES[data[0] % 4], std::string(reinterpret_cast<const char*>(data) + 1, size - 1));
    dns.tick(CloudflareDNS::CHECK_INTERVAL);
    return 0;
}
