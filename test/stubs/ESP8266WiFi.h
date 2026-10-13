// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "Arduino.h"
#include <functional>
#include <vector>
#include <utility>

// Raízes que uma conexão TLS recebeu (X509List::append)
namespace MockTLS {
    struct Trust {
        const uint8_t* first = nullptr;   // primeira raiz
        size_t         count = 0;
        size_t         bytes = 0;
    };
}

// ── Fila HTTP compartilhada (API via HTTPClient, download via TLS cru) ───────
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
        // TLS do cliente que fez a requisição (validação do certificado)
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
    inline bool                     rawAny = false; // rawHttp vale também para /releases/latest
    inline std::string              lastRaw;        // último pedido HTTP cru (TLS)

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

// Tipos globais — espelham o escopo do core ESP8266 3.1.2
struct WiFiEventStationModeGotIP        {};
struct WiFiEventStationModeDisconnected {};
using  WiFiEventHandler = int; // stub: valor de retorno ignorado

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

    // Mesma semântica do core: exatamente 4 octetos decimais 0–255
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
    IPAddress   _dns;                 // servidor DNS da rede (192.168.1.1)
    WiFiMode_t  _mode   = WIFI_STA;

    int         beginCalls      = 0;
    int         disconnectCalls = 0;
    std::string beginSSID, beginPass;
    std::string apSSID, apPass;
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

class WiFiClient {
public:
    virtual ~WiFiClient() {}
    virtual int    available()                 { return 0; }
    virtual int    read()                      { return -1; }
    virtual size_t readBytes(uint8_t*, size_t) { return 0; }
    virtual size_t write(const uint8_t*, size_t n) { return n; }
    virtual size_t print(const char* s) {
        return write(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
    }
    virtual bool   connected()                 { return false; }
    virtual int    connect(const char*, uint16_t) { return 0; }
    void setTimeout(unsigned long ms);
    virtual void stop(unsigned int = 0) {}
    size_t write_P(const char* p, size_t n) { return write(reinterpret_cast<const uint8_t*>(p), n); }
protected:
    WiFiClient* _owned = nullptr;
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
    inline Trust                     lastTrust;               // o X509List em si é temporário
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
    inline std::vector<uint16_t>     suites;                  // último setCiphers
    // Na N-ésima chamada de setBufferSizes (1-based) o maior bloco livre
    // cai para shrinkBlockTo: a alocação do BearSSL fragmentou a heap.
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
namespace MockTLS { inline WiFiClientSecure* live = nullptr; } // último que conectou

class WiFiClientSecure : public WiFiClient {
public:
    WiFiClientSecure() { _owned = MockTLS::nullOwned ? nullptr : &_ctx; }
    ~WiFiClientSecure() { if (MockTLS::live == this) MockTLS::live = nullptr; }

    // Dados que o servidor manda depois (fluxo contínuo)
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
