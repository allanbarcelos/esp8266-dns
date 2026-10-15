// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "Arduino.h"
#include <functional>
#include <vector>
#include <deque>
#include <memory>
#include <utility>

// Roots a TLS connection received (X509List::append)
namespace MockTLS {
    struct Trust {
        const uint8_t* first = nullptr;   // first root
        size_t         count = 0;
        size_t         bytes = 0;
    };
}

// ── Shared HTTP queue (API via HTTPClient, download via raw TLS) ─────────────
namespace MockHTTP {
    struct Response {
        int code = 200;
        std::string body;
        int size = -1;
        std::string location;
    };

    struct Request {
        std::string method, url, payload;
        std::vector<std::pair<std::string, std::string>> headers;
        // TLS of the client that made the request (certificate validation)
        MockTLS::Trust trust;
        long        x509Time = 0;
        bool        insecure = false;
    };

    inline std::vector<std::string> calledURLs;
    inline std::vector<Request>     requests;
    inline std::vector<Response>    queue;
    inline size_t                   qIdx = 0;
    inline std::vector<bool>        beginResults;
    inline size_t                   bIdx = 0;
    inline bool                     lastHTTP10 = false;
    inline std::string              lastUA;
    inline bool                     keepOpen = false;
    inline std::string              rawHttp;
    inline bool                     rawAny = false; // rawHttp also applies to /releases/latest
    inline std::string              lastRaw;        // last raw HTTP request (TLS)

    inline void reset() {
        calledURLs.clear(); requests.clear(); queue.clear(); qIdx = 0;
        beginResults.clear(); bIdx = 0;
        lastHTTP10 = false; lastUA.clear(); keepOpen = false;
        rawHttp.clear(); rawAny = false; lastRaw.clear();
    }

    inline void push(int code, const std::string& body = "", int size = -1,
                     const std::string& location = "") {
        queue.push_back({code, body, size, location});
    }

    inline void failNextBegin() { beginResults.push_back(false); }

    inline bool nextBegin() {
        return bIdx < beginResults.size() ? beginResults[bIdx++] : true;
    }

    inline Response next() {
        if (qIdx < queue.size()) return queue[qIdx++];
        return {};
    }
}

// Global types, mirroring the scope of ESP8266 core 3.1.2
struct WiFiEventStationModeGotIP        {};
struct WiFiEventStationModeDisconnected {};
using  WiFiEventHandler = int; // stub: return value ignored

enum WiFiMode_t { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };

enum WiFiSleepType_t { WIFI_NONE_SLEEP = 0, WIFI_LIGHT_SLEEP = 1, WIFI_MODEM_SLEEP = 2 };

enum wl_status_t {
    WL_IDLE_STATUS  = 0,
    WL_CONNECTED    = 3,
    WL_DISCONNECTED = 6,
};

class IPAddress {
public:
    uint32_t _addr = 0x0101A8C0; // 192.168.1.1

    IPAddress() = default;
    explicit IPAddress(uint32_t a) : _addr(a) {}
    bool     isSet() const             { return _addr != 0; }
    uint8_t  operator[](int i) const   { return (uint8_t)(_addr >> (8 * i)); }
    operator uint32_t() const          { return _addr; }

    String toString() const {
        char b[16];
        std::snprintf(b, sizeof(b), "%u.%u.%u.%u",
            _addr & 0xFF, (_addr >> 8) & 0xFF,
            (_addr >> 16) & 0xFF, (_addr >> 24) & 0xFF);
        return String(b);
    }

    // Same semantics as the core: exactly 4 decimal octets 0–255
    bool fromString(const String& s) {
        const char* p = s.c_str();
        uint32_t out = 0;
        for (int part = 0; part < 4; part++) {
            if (!isdigit((unsigned char)*p)) return false;
            unsigned v = 0;
            while (isdigit((unsigned char)*p)) {
                v = v * 10 + (*p++ - '0');
                if (v > 255) return false;
            }
            out |= v << (8 * part);
            if (part < 3) {
                if (*p != '.') return false;
                p++;
            }
        }
        if (*p != '\0') return false;
        _addr = out;
        return true;
    }
};

class WiFiClass {
public:
    wl_status_t _status = WL_CONNECTED;
    IPAddress   _dns;                 // network DNS server (192.168.1.1)
    WiFiMode_t  _mode   = WIFI_STA;

    int         beginCalls      = 0;
    int         disconnectCalls = 0;
    std::string beginSSID, beginPass;
    std::string apSSID, apPass;
    std::string hostName;
    bool        persistentFlag    = true;
    bool        autoReconnectFlag = false;
    WiFiSleepType_t sleepMode     = WIFI_MODEM_SLEEP;

    std::function<void(const WiFiEventStationModeGotIP&)>        gotIP;
    std::function<void(const WiFiEventStationModeDisconnected&)> disconnected;

    void reset() { *this = WiFiClass(); }

    wl_status_t status()            { return _status; }
    bool        isConnected()       { return _status == WL_CONNECTED; }
    WiFiMode_t  getMode()           { return _mode; }

    void begin(const char* s, const char* p) { beginCalls++; beginSSID = s; beginPass = p; }
    void disconnect()               { disconnectCalls++; }
    void setAutoReconnect(bool v)   { autoReconnectFlag = v; }
    void persistent(bool v)         { persistentFlag = v; }
    void setSleepMode(WiFiSleepType_t mode) { sleepMode = mode; }
    bool softAP(const char* s, const char* p) { apSSID = s; apPass = p; return true; }
    void mode(WiFiMode_t m)         { _mode = m; }
    bool hostname(const char* h)    { hostName = h; return true; }

    String    SSID()        { return String("Test<SSID>"); }
    IPAddress localIP()     { return IPAddress(); }
    IPAddress gatewayIP()   { return IPAddress(); }
    IPAddress subnetMask()  { return IPAddress(); }
    IPAddress dnsIP()       { return _dns; }
    String    macAddress()  { return String("AA:BB:CC:DD:EE:FF"); }
    int32_t   RSSI()        { return -70; }

    WiFiEventHandler onStationModeGotIP(
        std::function<void(const WiFiEventStationModeGotIP&)> f) { gotIP = f; return 1; }

    WiFiEventHandler onStationModeDisconnected(
        std::function<void(const WiFiEventStationModeDisconnected&)> f) { disconnected = f; return 2; }
};

inline WiFiClass WiFi;

// Connection accepted by WiFiServer: what the browser sends (rx) and what the
// device answers (tx). Shared between WiFiClient copies, like the
// core's ClientContext; the test keeps it alive (a raw pointer in
// the client: copies without reference counting).
struct MockConn {
    std::string rx, tx;
    size_t      pos    = 0;
    bool        open   = true;    // the browser has not closed yet
    bool        closed = false;   // the device called stop()
    int         writes = 0;
    // A slow client: once rx runs out, one byte of drip arrives every dripMs
    std::string   drip;
    unsigned long dripMs = 0;
    unsigned long dripAt = 0;

    void pump() {
        if (pos < rx.size() || drip.empty() || millis() - dripAt < dripMs) return;
        rx += drip[0];
        drip.erase(0, 1);
        dripAt = millis();
    }
};

class WiFiClient {
public:
    WiFiClient() = default;
    explicit WiFiClient(MockConn* c) : _conn(c) {}
    virtual ~WiFiClient() {}
    virtual int    available() {
        if (!_conn) return 0;
        _conn->pump();
        return (int)(_conn->rx.size() - _conn->pos);
    }
    virtual int    read() {
        if (!_conn || _conn->pos >= _conn->rx.size()) return -1;
        return (uint8_t)_conn->rx[_conn->pos++];
    }
    virtual size_t readBytes(uint8_t*, size_t) { return 0; }
    virtual size_t write(const uint8_t* b, size_t n) {
        if (_conn) {
            _conn->tx.append(reinterpret_cast<const char*>(b), n);
            _conn->writes++;
        }
        return n;
    }
    virtual size_t print(const char* s) {
        return write(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
    }
    virtual bool   connected()                 { return _conn && !_conn->closed && (_conn->open || available()); }
    virtual int    connect(const char*, uint16_t) { return 0; }
    void setTimeout(unsigned long ms);
    virtual void stop(unsigned int = 0) {
        if (_conn) _conn->closed = true;
        _conn = nullptr;
    }
    size_t write_P(const char* p, size_t n) { return write(reinterpret_cast<const uint8_t*>(p), n); }
    explicit operator bool() { return connected(); }
protected:
    WiFiClient* _owned = nullptr;
    MockConn*   _conn = nullptr;
};

// Queue of connections arriving at the port (the browser creates them in tests)
namespace MockServer {
    inline std::deque<std::shared_ptr<MockConn>> pending;
    inline std::vector<std::shared_ptr<MockConn>> accepted;   // alive until reset
    inline bool begun       = false;
    inline bool maxPending  = false;
    inline void reset() { pending.clear(); accepted.clear(); begun = false; maxPending = false; }
}

class WiFiServer {
public:
    explicit WiFiServer(uint16_t port) : port(port) {}
    void begin() { MockServer::begun = true; }
    WiFiClient accept() {
        if (MockServer::pending.empty()) return WiFiClient();
        MockServer::accepted.push_back(MockServer::pending.front());
        MockServer::pending.pop_front();
        MockConn* c = MockServer::accepted.back().get();
        return WiFiClient(c);
    }
    size_t hasClientData() {
        if (MockServer::pending.empty()) return 0;
        return MockServer::pending.front()->rx.size();
    }
    bool hasMaxPendingClients() { return MockServer::maxPending; }
    uint16_t port;
};

namespace BearSSL {
    class X509List {
    public:
        X509List() {}
        bool append(const uint8_t* der, size_t len) {
            if (!trust.count) trust.first = der;
            trust.count++;
            trust.bytes += len;
            return true;
        }
        MockTLS::Trust trust;
    };
}

namespace MockTLS {
    inline int                       insecureCalls = 0;
    inline Trust                     lastTrust;               // the X509List itself is temporary
    inline time_t                    lastTime      = 0;
    inline int                       timeoutCalls  = 0;
    inline unsigned long             lastTimeoutMs = 0;
    inline int                       bufIn         = 0;
    inline int                       bufOut        = 0;
    inline bool                      nullOwned     = false;
    inline bool                      failConnect   = false;
    inline int                       connectCalls  = 0;
    inline int                       failOnConnect = 0; // 1-based; 0 = off
    inline bool                      mfln          = false;
    inline int                       stopCalls     = 0;
    inline std::vector<uint16_t>     suites;                  // last setCiphers
    // On the Nth setBufferSizes call (1-based) the largest free block
    // drops to shrinkBlockTo: the BearSSL allocation fragmented the heap.
    inline int                       buffersCalls  = 0;
    inline int                       shrinkOnBuffers = 0;
    inline uint32_t                  shrinkBlockTo = 0;
    inline void reset() {
        buffersCalls = 0; shrinkOnBuffers = 0; shrinkBlockTo = 0;
        insecureCalls = 0; lastTrust = Trust{}; lastTime = 0;
        timeoutCalls = 0; lastTimeoutMs = 0;
        bufIn = 0; bufOut = 0; nullOwned = false;
        failConnect = false; connectCalls = 0; failOnConnect = 0;
        mfln = false; suites.clear(); stopCalls = 0;
    }
}

inline void WiFiClient::setTimeout(unsigned long ms) {
    MockTLS::timeoutCalls++;
    MockTLS::lastTimeoutMs = ms;
}

class WiFiClientSecure;
namespace MockTLS { inline WiFiClientSecure* live = nullptr; } // last one that connected

class WiFiClientSecure : public WiFiClient {
public:
    WiFiClientSecure() { _owned = MockTLS::nullOwned ? nullptr : &_ctx; }
    ~WiFiClientSecure() { if (MockTLS::live == this) MockTLS::live = nullptr; }

    // Data the server sends later (continuous stream)
    void inject(const std::string& data) { _rx += data; }
    bool isConnected() const { return _connected; }
    WiFiClient _ctx;
    MockTLS::Trust trust;
    time_t      x509Time = 0;
    bool        insecure = false;

    void setBufferSizes(int in, int out) {
        MockTLS::bufIn = in; MockTLS::bufOut = out;
        if (++MockTLS::buffersCalls == MockTLS::shrinkOnBuffers) ESP._maxBlock = MockTLS::shrinkBlockTo;
    }
    void setInsecure()                                 { MockTLS::insecureCalls++; insecure = true; }
    bool setCiphers(const uint16_t* list, int n)       { MockTLS::suites.assign(list, list + n); return true; }
    void setTrustAnchors(const BearSSL::X509List* ta)  { MockTLS::lastTrust = trust = ta->trust; }
    void setX509Time(time_t t)                         { MockTLS::lastTime = x509Time = t; }
    bool probeMaxFragmentLength(const char*, uint16_t, uint16_t) { return MockTLS::mfln; }

    int connect(const char* host, uint16_t) override {
        MockTLS::connectCalls++;
        if (MockTLS::failConnect) return 0;
        if (MockTLS::failOnConnect > 0 &&
            MockTLS::connectCalls == MockTLS::failOnConnect) return 0;
        _host = host ? host : "";
        _connected = true;
        MockTLS::live = this;
        _tx.clear();
        _rx.clear();
        _rxPos = 0;
        return 1;
    }

    size_t write(const uint8_t* buf, size_t n) override {
        _tx.append(reinterpret_cast<const char*>(buf), n);
        if (_tx.find("\r\n\r\n") != std::string::npos) {
            _serveResponse();
            _tx.clear();
        }
        return n;
    }

    size_t print(const char* s) override {
        return write(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
    }

    int available() override {
        return (int)(_rx.size() - _rxPos);
    }

    int read() override {
        if (_rxPos >= _rx.size()) return -1;
        return (uint8_t)_rx[_rxPos++];
    }

    size_t readBytes(uint8_t* buf, size_t n) override {
        size_t k = std::min(n, _rx.size() - _rxPos);
        std::memcpy(buf, _rx.data() + _rxPos, k);
        _rxPos += k;
        return k;
    }

    bool connected() override {
        if (!_connected) return false;
        if (MockHTTP::keepOpen) return true;
        return _rxPos < _rx.size();
    }

    void stop(unsigned int = 0) override {
        MockTLS::stopCalls++;
        _connected = false;
        _rx.clear();
        _rxPos = 0;
        _tx.clear();
    }

private:
    void _serveResponse() {
        std::string path = "/";
        auto getPos = _tx.find("GET ");
        if (getPos != std::string::npos) {
            auto end = _tx.find(' ', getPos + 4);
            if (end != std::string::npos)
                path = _tx.substr(getPos + 4, end - (getPos + 4));
        }
        std::string host = _host;
        auto hostPos = _tx.find("Host: ");
        if (hostPos != std::string::npos) {
            auto end = _tx.find("\r\n", hostPos);
            if (end != std::string::npos)
                host = _tx.substr(hostPos + 6, end - (hostPos + 6));
        }
        MockHTTP::calledURLs.push_back("https://" + host + path);
        MockHTTP::lastRaw = _tx;
        MockHTTP::lastHTTP10 = _tx.find("HTTP/1.0") != std::string::npos;
        if (_tx.find("User-Agent: ESP8266-dns") != std::string::npos)
            MockHTTP::lastUA = "ESP8266-dns";

        if (!MockHTTP::rawHttp.empty() &&
            (MockHTTP::rawAny || path.find("/releases/latest") == std::string::npos)) {
            _rx = std::move(MockHTTP::rawHttp);
            MockHTTP::rawHttp.clear();
            _rxPos = 0;
            return;
        }

        auto r = MockHTTP::next();
        int size = r.size;
        if (size == -2) size = -1;
        else if (size < 0) size = (int)r.body.size();

        std::string resp = "HTTP/1.0 " + std::to_string(r.code) + " x\r\n";
        resp += "Set-Cookie: x\r\n";
        if (!r.location.empty())
            resp += "Location: " + r.location + "\r\n";
        if (size >= 0)
            resp += "Content-Length: " + std::to_string(size) + "\r\n";
        resp += "\r\n";
        resp += r.body;
        _rx = std::move(resp);
        _rxPos = 0;
    }

    std::string _host;
    std::string _tx;
    std::string _rx;
    size_t      _rxPos = 0;
    bool        _connected = false;
};
