// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "Arduino.h"
#include <functional>
#include <map>
#include <string>
#include <utility>

#define HTTP_GET  0
#define HTTP_POST 1
#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)

// Servidor simulado: os testes disparam requisições com request() e
// inspecionam status, cabeçalhos e corpo produzidos pelos handlers.
class ESP8266WebServer {
public:
    using Handler = std::function<void()>;

    explicit ESP8266WebServer(int port) : port(port) { instance() = this; }

    static ESP8266WebServer*& instance() { static ESP8266WebServer* p = nullptr; return p; }

    void on(const char* path, int method, Handler h) { routes[{path, method}] = h; }
    void begin()        { started = true; }
    void handleClient() { handleCalls++; }

    // ── API usada pelos handlers ─────────────────────────────────────────────
    void setContentLength(size_t len) { contentLength = len; }
    void send(int c, const char* type, const String& body) {
        code = c; contentType = type; out += body._s;
    }
    // Como no core: em resposta chunked, pedaço vazio é o terminador
    // ("0\r\n\r\n") e o que vier depois não chega ao navegador.
    void send_P(int c, const char* type, const char* data, size_t len) {
        code = c; contentType = type; out.assign(data, len);
    }
    void sendContent(const String& s)   { _chunk(s._s); }
    void sendContent(const char* s)     { _chunk(s); }
    void sendContent_P(const char* s)   { _chunk(s); }
    void sendHeader(const char* k, const String& v) { headers[k] = v._s; }

    bool authenticate(const char* u, const char* p) {
        lastAuthUser = u; lastAuthPass = p;
        return authOk;
    }
    void requestAuthentication() { authRequested = true; code = 401; }

    bool   hasArg(const String& k) { return args.count(k._s) > 0; }
    String arg(const String& k)    { auto it = args.find(k._s); return it == args.end() ? String() : String(it->second); }

    // ── Controle dos testes ──────────────────────────────────────────────────
    bool request(int method, const std::string& path,
                 std::map<std::string, std::string> a = {}) {
        out.clear(); headers.clear(); code = 0; contentType.clear();
        authRequested = false; contentLength = 0; ended = false;
        args = std::move(a);
        auto it = routes.find({path, method});
        if (it == routes.end()) return false;
        it->second();
        return true;
    }

    int  port;
    bool started     = false;
    int  handleCalls = 0;
    bool authOk      = true;

    std::map<std::pair<std::string, int>, Handler> routes;
    std::map<std::string, std::string>             args;
    std::map<std::string, std::string>             headers;
    std::string out, contentType, lastAuthUser, lastAuthPass;
    int    code          = 0;
    size_t contentLength = 0;
    bool   authRequested = false;
    bool   ended         = false;

private:
    void _chunk(const std::string& s) {
        if (ended) return;
        if (contentLength == CONTENT_LENGTH_UNKNOWN && s.empty()) {
            ended = true;
            return;
        }
        out += s;
    }
};
