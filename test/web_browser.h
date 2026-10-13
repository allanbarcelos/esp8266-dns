// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <functional>
#include <map>
#include <string>
#include <base64.h>
#include "HttpServer.h"

constexpr int HTTP_GET  = HttpServer::GET;
constexpr int HTTP_POST = HttpServer::POST;

// Navegador dos testes: escreve o pedido HTTP numa conexão nova, roda um
// tick do servidor e lê a resposta crua (status, cabeçalhos e corpo).
// Sem Authorization explícito, manda o login do painel (basicUser/basicPass)
// se authOk. Argumentos vão na query (GET) ou no corpo urlencoded (POST);
// "plain" vira o corpo cru, como o curl -d.
struct MockBrowser {
    using Map = std::map<std::string, std::string>;

    explicit MockBrowser(std::function<void()> t) : tick(std::move(t)) {}

    std::function<void()> tick;
    bool        authOk    = true;
    std::string basicUser = "admin", basicPass = "senha-forte";

    int         code = 0;
    std::string out, contentType, raw;
    Map         headers;
    std::shared_ptr<MockConn> conn;

    static std::string encode(const std::string& s) {
        std::string o;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                o += (char)c;
            } else if (c == ' ') {
                o += '+';
            } else {
                char b[4];
                std::snprintf(b, sizeof(b), "%%%02X", c);
                o += b;
            }
        }
        return o;
    }

    static std::string form(const Map& a) {
        std::string o;
        for (auto& kv : a) {
            if (kv.first == "plain") continue;
            if (!o.empty()) o += '&';
            o += encode(kv.first) + "=" + encode(kv.second);
        }
        return o;
    }

    // Pedido já escrito; devolve false se a rota não existe (404 vazio)
    bool send(const std::string& request) {
        conn = std::make_shared<MockConn>();
        conn->rx = request;
        MockServer::pending.push_back(conn);
        tick();
        parse(conn->tx);
        return !(code == 404 && out.empty());
    }

    bool request(int method, const std::string& path, Map a = {}, Map h = {}) {
        std::string body = a.count("plain") ? a["plain"] : form(a);
        std::string target = path;
        if (method == HTTP_GET && !body.empty()) target += "?" + body;
        std::string r = std::string(method == HTTP_GET ? "GET " : "POST ") + target +
                        " HTTP/1.1\r\nHost: esp\r\n";
        if (!h.count("Authorization") && authOk)
            h["Authorization"] = std::string("Basic ") +
                base64::encode(String((basicUser + ":" + basicPass).c_str())).c_str();
        for (auto& kv : h) r += kv.first + ": " + kv.second + "\r\n";
        if (method == HTTP_POST) {
            r += "Content-Type: application/x-www-form-urlencoded\r\n";
            r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        } else {
            r += "\r\n";
        }
        return send(r);
    }

    void parse(const std::string& tx) {
        raw = tx;
        code = 0;
        out.clear();
        contentType.clear();
        headers.clear();
        size_t end = tx.find("\r\n\r\n");
        if (tx.compare(0, 9, "HTTP/1.1 ") != 0 || end == std::string::npos) return;
        code = std::atoi(tx.c_str() + 9);
        size_t at = tx.find("\r\n") + 2;
        while (at < end) {
            size_t nl = tx.find("\r\n", at);
            std::string line = tx.substr(at, nl - at);
            size_t colon = line.find(": ");
            headers[line.substr(0, colon)] = line.substr(colon + 2);
            at = nl + 2;
        }
        contentType = headers["Content-Type"];
        out = tx.substr(end + 4);
    }
};
