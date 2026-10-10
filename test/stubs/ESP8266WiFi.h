#pragma once
#include "Arduino.h"
#include <functional>

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
    IPAddress dnsIP()       { return IPAddress(); }
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
    virtual size_t readBytes(uint8_t*, size_t) { return 0; }
    virtual bool   connected()                 { return false; }
    void setTimeout(unsigned long ms);
    void stop(unsigned int = 0) {}
protected:
    WiFiClient* _owned = nullptr;
};

namespace BearSSL {
    class X509List {
    public:
        explicit X509List(const char* pem) : pem(pem) {}
        const char* pem;
    };
}

namespace MockTLS {
    inline int                       insecureCalls = 0;
    inline const char*               lastTrustPem  = nullptr; // o X509List em si é temporário
    inline time_t                    lastTime      = 0;
    inline int                       timeoutCalls  = 0;
    inline unsigned long             lastTimeoutMs = 0;
    inline int                       bufIn         = 0;
    inline int                       bufOut        = 0;
    inline bool                      nullOwned     = false;
    inline void reset() {
        insecureCalls = 0; lastTrustPem = nullptr; lastTime = 0;
        timeoutCalls = 0; lastTimeoutMs = 0;
        bufIn = 0; bufOut = 0; nullOwned = false;
    }
}

inline void WiFiClient::setTimeout(unsigned long ms) {
    MockTLS::timeoutCalls++;
    MockTLS::lastTimeoutMs = ms;
}

class WiFiClientSecure : public WiFiClient {
public:
    WiFiClientSecure() { _owned = MockTLS::nullOwned ? nullptr : &_ctx; }
    WiFiClient _ctx;

    void setBufferSizes(int in, int out)               { MockTLS::bufIn = in; MockTLS::bufOut = out; }
    void setInsecure()                                 { MockTLS::insecureCalls++; }
    void setTrustAnchors(const BearSSL::X509List* ta)  { MockTLS::lastTrustPem = ta->pem; }
    void setX509Time(time_t t)                         { MockTLS::lastTime = t; }
};
