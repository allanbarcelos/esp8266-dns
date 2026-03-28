#pragma once
#include "Arduino.h"
#include <functional>

// Tipos globais — espelham o escopo do core ESP8266 3.1.2
struct WiFiEventStationModeGotIP      { String ip; };
struct WiFiEventStationModeDisconnected {};
using  WiFiEventHandler = int; // stub: valor de retorno ignorado

typedef enum {
    WIFI_EVENT_STAMODE_DISCONNECTED = 0,
    WIFI_EVENT_STAMODE_GOT_IP       = 1,
} WiFiEvent_t;

enum WiFiMode_t { WIFI_AP = 1, WIFI_STA = 2 };
using WiFiMode = WiFiMode_t;

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
        std::snprintf(b, sizeof(b), "%d.%d.%d.%d",
            _addr & 0xFF, (_addr >> 8) & 0xFF,
            (_addr >> 16) & 0xFF, (_addr >> 24) & 0xFF);
        return String(b);
    }
    operator bool() const { return _addr != 0; }
};

class WiFiClass {
public:
    wl_status_t _status = WL_CONNECTED;
    WiFiMode    _mode   = WIFI_STA;

    wl_status_t status()      { return _status; }
    bool        isConnected() { return _status == WL_CONNECTED; }
    WiFiMode    getMode()     { return _mode; }

    void begin(const char*, const char*) {}
    void disconnect()            {}
    void setAutoReconnect(bool)  {}
    void persistent(bool)        {}
    bool softAP(const char*)     { return true; }
    void mode(WiFiMode m)        { _mode = m; }

    String    SSID()        { return String("TestSSID"); }
    IPAddress localIP()     { return IPAddress(); }
    IPAddress gatewayIP()   { return IPAddress(); }
    IPAddress subnetMask()  { return IPAddress(); }
    IPAddress dnsIP()       { return IPAddress(); }
    String    macAddress()  { return String("AA:BB:CC:DD:EE:FF"); }
    int32_t   RSSI()        { return -70; }

    bool hostByName(const char*, IPAddress& ip) { ip._addr = 1; return true; }

    void onEvent(std::function<void(WiFiEvent_t)>) {}

    WiFiEventHandler onStationModeGotIP(
        std::function<void(const WiFiEventStationModeGotIP&)>) { return 0; }

    WiFiEventHandler onStationModeDisconnected(
        std::function<void(const WiFiEventStationModeDisconnected&)>) { return 0; }
};

inline WiFiClass WiFi;

// WiFiClient mínimo (sem herança de Stream para não introduzir ambiguidade)
class WiFiClient {
public:
    virtual ~WiFiClient() {}
    virtual bool   connect(const char*, uint16_t) { return true; }
    virtual bool   connected()                     { return true; }
    virtual int    available()                     { return 0; }
    virtual int    read()                          { return -1; }
    virtual size_t readBytes(uint8_t*, size_t)     { return 0; }
    virtual void   stop()                          {}
};

class WiFiClientSecure : public WiFiClient {
public:
    void setInsecure()                {}
    void setFingerprint(const char*)  {}
};
