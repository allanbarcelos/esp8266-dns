#pragma once
#include "Arduino.h"
#include <functional>
#include <map>
#include <string>

#define HTTP_GET  0
#define HTTP_POST 1
#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)

class ESP8266WebServer {
public:
    explicit ESP8266WebServer(int) {}

    void on(const char*, int, std::function<void()>) {}
    void on(const char*, std::function<void()>)      {}
    void begin()        {}
    void handleClient() {}

    void setContentLength(size_t)                   {}
    void send(int, const char*, const String& = "") {}
    void sendContent(const String&)                 {}
    void sendContent(const char*)                   {}
    void sendContent_P(const char*)                 {}
    void sendHeader(const char*, const String&)     {}

    bool   authenticate(const char*, const char*)   { return true; }
    void   requestAuthentication()                  {}
    bool   hasArg(const String&)                    { return false; }
    String arg(const String&)                       { return String(); }
    String arg(const char*)                         { return String(); }
};
