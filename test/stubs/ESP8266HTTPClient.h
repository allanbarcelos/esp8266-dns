// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "Arduino.h"
#include "ESP8266WiFi.h"

#define HTTP_CODE_OK 200
#define HTTPC_FORCE_FOLLOW_REDIRECTS 1

// ── Stream que serve o body da resposta para ArduinoJson ─────────────────────
class MockStream {
    std::string _data;
    size_t      _pos = 0;
public:
    MockStream() {}
    explicit MockStream(const std::string& d) : _data(d), _pos(0) {}
    int read() { return _pos < _data.size() ? (uint8_t)_data[_pos++] : -1; }
};

// ── Cliente que entrega o body como stream de bytes (download OTA) ───────────
class MockBodyClient : public WiFiClient {
public:
    std::string data;
    size_t      pos = 0;

    int available() override { return (int)(data.size() - pos); }
    bool connected() override {
        if (MockHTTP::keepOpen) return true;
        return pos < data.size();
    }
    size_t readBytes(uint8_t* buf, size_t n) override {
        size_t k = std::min(n, data.size() - pos);
        std::memcpy(buf, data.data() + pos, k);
        pos += k;
        return k;
    }
};

// ── HTTPClient mock ───────────────────────────────────────────────────────────
class HTTPClient {
public:
    bool begin(WiFiClient& client, const String& url) {
        MockHTTP::calledURLs.push_back(url.c_str());
        _req = MockHTTP::Request();
        _req.url = url.c_str();
        if (auto* tls = dynamic_cast<WiFiClientSecure*>(&client)) {
            _req.trust = tls->trust;
            _req.x509Time = (long)tls->x509Time;
            _req.insecure = tls->insecure;
        }
        return MockHTTP::nextBegin();
    }

    void setTimeout(int) {}
    void setFollowRedirects(int) {}
    void setRedirectLimit(uint16_t) {}
    void useHTTP10(bool v = true) { MockHTTP::lastHTTP10 = v; }
    void setUserAgent(const String& ua) { MockHTTP::lastUA = ua.c_str(); }
    bool connected() { return _client.connected() || _client.available() > 0; }
    void addHeader(const String& k, const String& v) {
        _req.headers.push_back({k.c_str(), v.c_str()});
    }

    int GET()                  { return _perform("GET", ""); }
    int POST(const String& p)  { return _perform("POST", p.c_str()); }
    int PATCH(const String& p) { return _perform("PATCH", p.c_str()); }

    String      getString()    { return String(_body.c_str()); }
    int         getSize()      { return _size; }
    MockStream& getStream()    { return _stream; }
    WiFiClient* getStreamPtr() { return &_client; }

    void end() { ended++; }

    int ended = 0;

private:
    int _perform(const char* method, const std::string& payload) {
        _req.method  = method;
        _req.payload = payload;
        MockHTTP::requests.push_back(_req);

        auto r = MockHTTP::next();
        _body   = r.body;
        if (r.size == -2) _size = -1;
        else if (r.size < 0) _size = (int)r.body.size();
        else _size = r.size;
        _stream = MockStream(r.body);
        _client.data = r.body;
        _client.pos  = 0;
        return r.code;
    }

    MockHTTP::Request _req;
    std::string       _body;
    int               _size = 0;
    MockStream        _stream;
    MockBodyClient    _client;
};
