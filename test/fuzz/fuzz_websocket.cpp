// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// What the relay's server sends to the WebSocket client (WsClient.h): the
// answer to the upgrade, then frames of any shape.
#include "../test_support.h"
#include "WsClient.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    resetMocks();
    MockConn conn;
    conn.rx.assign(reinterpret_cast<const char*>(data), size);
    conn.open = false;
    WiFiClient c(&conn);
    WsClient ws(c);
    if (ws.handshake("h", "/", "") != 101) return 0;
    char buf[64];
    while (ws.frame()) {
        if (ws.op() == WsClient::TEXT) ws.read(buf, sizeof(buf));
        else ws.skip();
        if (ws.error()) break;
    }
    return 0;
}
