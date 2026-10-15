// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// The answer to a one-shot HTTPS request (https::request): status line,
// headers and body, as the server sends them.
#include "../test_support.h"
#include "TlsClient.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    resetMocks();
    MockHTTP::keepOpen = data[0] & 1;     // the server keeps the connection open, or closes it
    MockHTTP::rawHttp.assign(reinterpret_cast<const char*>(data) + 1, size - 1);
    if (MockHTTP::rawHttp.empty()) return 0;
    TlsClient c;
    String body;
    https::request(c, "h", "GET", "/", nullptr, nullptr, 50, &body, 256);
    return 0;
}
