#pragma once
// Não definimos ARDUINO aqui para evitar ativar polyfills PROGMEM do ArduinoJson.
// O suporte a String e Stream é habilitado via build_flags no platformio.ini:
//   -DARDUINOJSON_ENABLE_ARDUINO_STRING=1
//   -DARDUINOJSON_ENABLE_PROGMEM=0

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <ctime>
#include <string>
#include <cctype>
#include <algorithm>
#include <vector>

// ── Relógio controlável nos testes ──────────────────────────────────────────
namespace mock {
    inline unsigned long _millis = 0;
}
inline unsigned long millis()                        { return mock::_millis; }
inline void          setMillis(unsigned long ms)     { mock::_millis = ms; }
inline void          advanceMillis(unsigned long ms) { mock::_millis += ms; }

inline void delay(unsigned long ms) { mock::_millis += ms; }
inline void yield() {}

// ── SNTP ─────────────────────────────────────────────────────────────────────
namespace mock {
    inline int         configTimeCalls = 0;
    inline std::string ntpServer1;
}
inline void configTime(int, int, const char* s1, const char* = nullptr, const char* = nullptr) {
    mock::configTimeCalls++;
    mock::ntpServer1 = s1 ? s1 : "";
}

// ── strlcpy portátil (nem toda libc do host oferece) ─────────────────────────
inline size_t mock_strlcpy(char* dst, const char* src, size_t size) {
    size_t n = std::strlen(src);
    if (size) {
        size_t k = n < size - 1 ? n : size - 1;
        std::memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}
#define strlcpy mock_strlcpy

// ── PROGMEM (no-op no host) ──────────────────────────────────────────────────
#define PROGMEM
#define pgm_read_ptr(p)  (*(p))
#define strcpy_P(d, s)   strcpy((d), (s))

#define HEX 16
#define DEC 10

// ── String ───────────────────────────────────────────────────────────────────
class String {
public:
    std::string _s;

    String() {}
    String(const char* s)          : _s(s ? s : "") {}
    String(const std::string& s)   : _s(s) {}
    String(char c)                 { _s += c; }
    String(int v)                  : _s(std::to_string(v)) {}
    String(long v)                 : _s(std::to_string(v)) {}
    String(unsigned long v)        : _s(std::to_string(v)) {}
    String(unsigned int v, int base = DEC) {
        char b[16];
        std::snprintf(b, sizeof(b), base == HEX ? "%x" : "%u", v);
        _s = b;
    }
    String(float v, int dec = 2) {
        char b[32]; std::snprintf(b, sizeof(b), "%.*f", dec, (double)v); _s = b;
    }

    const char* c_str()   const { return _s.c_str(); }
    size_t      length()  const { return _s.size(); }
    bool        isEmpty() const { return _s.empty(); }

    bool concat(const String& s) { _s += s._s; return true; }
    bool concat(const char*   s) { if (s) _s += s; return true; }
    bool concat(char           c) { _s += c;  return true; }

    void trim() {
        size_t b = _s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) { _s.clear(); return; }
        _s = _s.substr(b, _s.find_last_not_of(" \t\r\n") - b + 1);
    }

    bool operator==(const String& o) const { return _s == o._s; }
    bool operator!=(const String& o) const { return _s != o._s; }
    bool operator==(const char*   o) const { return _s == o; }
    bool operator!=(const char*   o) const { return _s != o; }

    String& operator=(const char*   s) { _s = s ? s : ""; return *this; }
    String& operator=(const String& o) { _s = o._s;       return *this; }

    String& operator+=(const String& o) { _s += o._s; return *this; }
    String& operator+=(const char*   o) { _s += o;    return *this; }
    String& operator+=(char          c) { _s += c;    return *this; }

    String operator+(const String& o) const { String r; r._s = _s + o._s; return r; }
    String operator+(const char*   o) const { String r; r._s = _s + o;    return r; }
    String operator+(char          c) const { String r; r._s = _s + c;    return r; }
};

inline String operator+(const char* a, const String& b) { return String(a) + b; }

// ── Print / Stream (necessários para ArduinoJson reconhecer os tipos) ────────
class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t* buf, size_t n) {
        size_t w = 0;
        while (n--) w += write(*buf++);
        return w;
    }
};

// ── Serial ────────────────────────────────────────────────────────────────────
struct SerialClass {
    int  baud = 0;
    void begin(int b)             { baud = b; }
    void println(const char* s)   { std::printf("%s\n", s); }
    void printf(const char* fmt, ...) {
        va_list a; va_start(a, fmt); std::vprintf(fmt, a); va_end(a);
    }
};

// ── ESP ───────────────────────────────────────────────────────────────────────
struct EspClass {
    uint32_t    getFreeHeap()           { return _heap; }
    uint8_t     getHeapFragmentation()  { return _frag; }
    uint32_t    getMaxFreeBlockSize()   { return _maxBlock; }
    String      getResetInfo()          { return _resetInfo; }
    uint32_t    getFlashChipSize()      { return 1048576; }
    uint32_t    getSketchSize()         { return 300000; }
    uint32_t    getFreeSketchSpace()    { return 300000; }
    uint32_t    getCpuFreqMHz()         { return 80; }
    uint32_t    getChipId()             { return 0xDEAD; }
    const char* getSdkVersion()         { return "2.2.2"; }
    String      getResetReason()        { return String("Power On"); }
    void        restart()               { _restarts++; }
    uint32_t    random()                { return _rand = _rand * 1103515245u + 12345u; }

    // Controláveis nos testes
    uint32_t _heap     = 50000;
    uint32_t _maxBlock = 40000;
    String   _resetInfo = "Power On";
    uint8_t  _frag     = 10;
    int      _restarts = 0;
    uint32_t _rand     = 1;

    void reset() { *this = EspClass(); }
};

inline SerialClass Serial;
inline EspClass    ESP;
