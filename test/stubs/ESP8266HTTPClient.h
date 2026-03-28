#pragma once
#include "Arduino.h"
#include "ESP8266WiFi.h"
#include <string>
#include <vector>
#include <utility>

#define HTTP_CODE_OK 200

// ── Estado compartilhado do mock ─────────────────────────────────────────────
namespace MockHTTP {
    // Fila de respostas: cada chamada GET/POST/PATCH consome a próxima
    struct Response { int code = 200; std::string body; };

    inline std::vector<std::string>  calledURLs;
    inline std::vector<Response>     queue;
    inline size_t                    qIdx = 0;

    inline void reset() { calledURLs.clear(); queue.clear(); qIdx = 0; }

    inline void push(int code, const std::string& body = "") {
        queue.push_back({code, body});
    }

    inline Response next() {
        if (qIdx < queue.size()) return queue[qIdx++];
        return {200, ""};
    }
}

// ── Stream que serve o body da resposta para ArduinoJson ─────────────────────
class MockStream {
    std::string _data;
    size_t      _pos = 0;
public:
    MockStream() {}
    explicit MockStream(const std::string& d) : _data(d), _pos(0) {}

    int    available() { return (int)(_data.size() - _pos); }
    int    read()      { return _pos < _data.size() ? (uint8_t)_data[_pos++] : -1; }
    size_t readBytes(uint8_t* buf, size_t n) {
        size_t k = std::min(n, _data.size() - _pos);
        std::memcpy(buf, _data.data() + _pos, k);
        _pos += k;
        return k;
    }
};

// ── HTTPClient mock ───────────────────────────────────────────────────────────
class HTTPClient {
public:
    bool begin(WiFiClient&,       const String& url) { return _open(url); }
    bool begin(WiFiClientSecure&, const String& url) { return _open(url); }

    void setTimeout(int) {}
    void addHeader(const char*, const String&) {}
    void addHeader(const String&, const String&) {}

    int GET() {
        auto r = MockHTTP::next();
        _lastCode = r.code;
        _stream   = MockStream(r.body);
        _body     = r.body;
        return _lastCode;
    }
    int POST(const String&)  { return GET(); }
    int PATCH(const String&) { return GET(); }

    String      getString()              { return String(_body.c_str()); }
    int         getSize()                { return (int)_body.size(); }
    MockStream& getStream()              { return _stream; }
    WiFiClient* getStreamPtr()           { return &_client; }

    void end() {}

private:
    bool _open(const String& url) {
        MockHTTP::calledURLs.push_back(url.c_str());
        return true;
    }

    int        _lastCode = 0;
    std::string _body;
    MockStream  _stream;
    WiFiClient  _client;
};
