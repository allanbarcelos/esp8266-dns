#pragma once
#include "Arduino.h"
#include "ESP8266WiFi.h"
#include <string>
#include <vector>
#include <utility>

#define HTTP_CODE_OK 200
#define HTTPC_FORCE_FOLLOW_REDIRECTS 1

// ── Estado compartilhado do mock ─────────────────────────────────────────────
namespace MockHTTP {
    // Fila de respostas: cada GET/POST/PATCH consome a próxima.
    // size < 0 → getSize() devolve body.size(); senão devolve size.
    struct Response { int code = 200; std::string body; int size = -1; };

    struct Request {
        std::string method, url, payload;
        std::vector<std::pair<std::string, std::string>> headers;
    };

    inline std::vector<std::string> calledURLs;
    inline std::vector<Request>     requests;
    inline std::vector<Response>    queue;
    inline size_t                   qIdx = 0;
    inline std::vector<bool>        beginResults; // consumido a cada begin(); vazio = true
    inline size_t                   bIdx = 0;

    inline void reset() {
        calledURLs.clear(); requests.clear(); queue.clear(); qIdx = 0;
        beginResults.clear(); bIdx = 0;
    }

    inline void push(int code, const std::string& body = "", int size = -1) {
        queue.push_back({code, body, size});
    }

    inline void failNextBegin() { beginResults.push_back(false); }

    inline bool nextBegin() {
        return bIdx < beginResults.size() ? beginResults[bIdx++] : true;
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
    int read() { return _pos < _data.size() ? (uint8_t)_data[_pos++] : -1; }
};

// ── Cliente que entrega o body como stream de bytes (download OTA) ───────────
class MockBodyClient : public WiFiClient {
public:
    std::string data;
    size_t      pos = 0;

    int available() override { return (int)(data.size() - pos); }
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
    bool begin(WiFiClient&, const String& url) {
        MockHTTP::calledURLs.push_back(url.c_str());
        _req = MockHTTP::Request();
        _req.url = url.c_str();
        return MockHTTP::nextBegin();
    }

    void setTimeout(int) {}
    void setFollowRedirects(int) {}
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
        _size   = r.size < 0 ? (int)r.body.size() : r.size;
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
