// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Resposta qualquer do DNS da rede à pergunta do reverso (PTR). Os dois
// primeiros bytes são o id, como no pacote: o id da pergunta é 0x0001.
#include "../test_support.h"
#include "ReverseDNS.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char out[64];
    ReverseDNS::parse(data, size, 1, out, sizeof(out));
    ReverseDNS::parse(data, size, 1, out, 8);   // saída curta
    return 0;
}
