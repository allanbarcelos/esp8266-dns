// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Any answer from the network DNS to the reverse (PTR) query. The first
// two bytes are the id, as in the packet: the query id is 0x0001.
#include "../test_support.h"
#include "ReverseDNS.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char out[64];
    ReverseDNS::parse(data, size, 1, out, sizeof(out));
    ReverseDNS::parse(data, size, 1, out, 8);   // short output
    return 0;
}
