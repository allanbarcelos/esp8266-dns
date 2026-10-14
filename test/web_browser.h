// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <functional>
#include <map>
#include <string>
#include <base64.h>
#include "HttpServer.h"
#include "i18n_en.h"

constexpr int HTTP_GET  = HttpServer::GET;
constexpr int HTTP_POST = HttpServer::POST;

// Test browser: writes the HTTP request on a new connection, runs one
// server tick and reads the raw response (status, headers and body). An
// HTML body is read in English, as i18n.js writes it: out has the text and
// raw keeps the markers.
// Without an explicit Authorization, sends the panel login (basicUser/basicPass)
// if authOk. Arguments go in the query (GET) or the urlencoded body (POST);
// "plain" becomes the raw body, like curl -d.
struct MockBrowser {
    using Map = std::map<std::string, std::string>;

    explicit MockBrowser(std::function<void()> t) : tick(std::move(t)) {}

    std::function<void()> tick;
    bool        authOk    = true;
    std::string basicUser = "admin", basicPass = "strong-pass";

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

    // Request already written; returns false if the route does not exist (empty 404)
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
        if (contentType == "text/html") out = english(out);
    }

    // I18n.h markers (` plus two base-62 characters) → English text. A text
    // with {0} takes the rest of its text node, up to the next tag or marker.
    static std::string english(const std::string& s) {
        static const std::string B62 =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::string o;
        for (size_t i = 0; i < s.size(); i++) {
            size_t a = i + 2 < s.size() ? B62.find(s[i + 1]) : std::string::npos;
            size_t b = i + 2 < s.size() ? B62.find(s[i + 2]) : std::string::npos;
            size_t n = a * 62 + b;
            if (s[i] != '`' || a == std::string::npos || b == std::string::npos ||
                n >= sizeof(I18N_EN) / sizeof(I18N_EN[0])) {
                o += s[i];   // not a marker: a ` typed by someone stays
                continue;
            }
            std::string t = I18N_EN[n];
            i += 2;
            size_t at = t.find("{0}");
            if (at != std::string::npos) {
                size_t stop = s.find_first_of("<`", i + 1);
                if (stop == std::string::npos) stop = s.size();
                t.replace(at, 3, s.substr(i + 1, stop - i - 1));
                i = stop - 1;
            }
            o += t;
        }
        return o;
    }
};
