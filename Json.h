// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include <LittleFS.h>

// A small JSON reader and writer, in place of ArduinoJson (~7 KB plus the
// code it inlined at each use). The device reads its own files (config.json,
// iot.state) and short answers from outside (Cloudflare, the ntfy stream),
// and writes only flat objects.
//
// Reading never changes the text. object() checks that the whole object is
// valid JSON before the first callback, so a broken file changes nothing,
// then calls back with each top-level member. A nested object or array comes
// whole (members(), first()) and is read the same way. Strings keep their
// escapes until copy() or str() decodes them into their destination.
// Numbers are integers of up to 32 bits with a sign; a fraction, an exponent
// or more digits make a NUMBER, which no field accepts.
namespace json {

enum Kind : uint8_t { BAD, STRING, INTEGER, NUMBER, BOOL, NUL, OBJECT, ARRAY };

constexpr int MAX_DEPTH = 10;   // nesting, like ArduinoJson: bounded stack

struct Value {
    uint8_t     kind = BAD;
    bool        neg  = false;    // INTEGER: negative
    uint32_t    mag  = 0;        // INTEGER: magnitude; BOOL: 1 for true
    const char* at   = nullptr;  // STRING: after the opening quote; OBJECT, ARRAY: the bracket

    // An INTEGER within [lo, hi]: its bits (two's complement) in out
    bool fits(long lo, uint32_t hi, uint32_t& out) const {
        if (kind != INTEGER || (neg ? mag > (uint32_t)-lo : mag > hi)) return false;
        out = neg ? 0u - mag : mag;
        return true;
    }
};

// key: the member's name as written (escapes are not decoded), len bytes
using Fn = void (*)(void* ctx, const char* key, size_t len, const Value& v);

// ── Reading ──────────────────────────────────────────────────────────────────

inline const char* _ws(const char* p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return p;
}

inline int _hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// p after the opening quote; the position after the closing one, or null
inline const char* _string(const char* p) {
    for (;;) {
        char c = *p++;
        if (c == '"') return p;
        if (c == '\0') return nullptr;
        if (c != '\\') continue;
        c = *p++;
        if (c == 'u') {
            for (int i = 0; i < 4; i++)
                if (_hex(*p++) < 0) return nullptr;
        } else if (!c || !strchr("\"\\/bfnrt", c)) {
            return nullptr;
        }
    }
}

inline const char* _digits(const char* p) {
    if (*p < '0' || *p > '9') return nullptr;
    while (*p >= '0' && *p <= '9') p++;
    return p;
}

inline const char* _number(const char* p, Value& v) {
    v.neg = *p == '-';
    if (v.neg) p++;
    const char* end = _digits(p);
    if (!end) return nullptr;
    v.kind = INTEGER;
    for (; p != end; p++) {
        uint32_t d = (uint32_t)(*p - '0');
        if (v.mag > (0xFFFFFFFFu - d) / 10) v.kind = NUMBER;   // more than 32 bits
        v.mag = v.mag * 10 + d;
    }
    if (*p == '.') {
        v.kind = NUMBER;
        p = _digits(p + 1);
        if (!p) return nullptr;
    }
    if (*p == 'e' || *p == 'E') {
        v.kind = NUMBER;
        p++;
        if (*p == '+' || *p == '-') p++;
        p = _digits(p);
    }
    return p;
}

inline const char* _value(const char* p, Value& v, int depth);

// p at the '{'; the position after the '}', or null
inline const char* _members(const char* p, Fn fn, void* ctx, int depth) {
    p = _ws(p + 1);
    if (*p == '}') return p + 1;
    for (;;) {
        if (*p != '"') return nullptr;
        const char* key = p + 1;
        const char* end = _string(key);
        if (!end) return nullptr;
        p = _ws(end);
        if (*p != ':') return nullptr;
        Value v;
        p = _value(p + 1, v, depth);
        if (!p) return nullptr;
        if (fn) fn(ctx, key, (size_t)(end - 1 - key), v);
        p = _ws(p);
        if (*p == '}') return p + 1;
        if (*p != ',') return nullptr;
        p = _ws(p + 1);
    }
}

// p at the '['; the position after the ']', or null. first gets element 0.
inline const char* _items(const char* p, int depth, Value* first) {
    p = _ws(p + 1);
    if (*p == ']') return p + 1;
    for (bool zero = true;; zero = false) {
        Value v;
        p = _value(p, v, depth);
        if (!p) return nullptr;
        if (zero && first) *first = v;
        p = _ws(p);
        if (*p == ']') return p + 1;
        if (*p != ',') return nullptr;
        p++;
    }
}

inline const char* _value(const char* p, Value& v, int depth) {
    p = _ws(p);
    v.at = p;
    char c = *p;
    if (c == '"') {
        v.kind = STRING;
        v.at = p + 1;
        return _string(p + 1);
    }
    if (c == '{' || c == '[') {
        if (depth >= MAX_DEPTH) return nullptr;
        v.kind = c == '{' ? OBJECT : ARRAY;
        return c == '{' ? _members(p, nullptr, nullptr, depth + 1) : _items(p, depth + 1, nullptr);
    }
    if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
        v.kind = BOOL;
        v.mag = c == 't';
        return p + (c == 't' ? 4 : 5);
    }
    if (!strncmp(p, "null", 4)) {
        v.kind = NUL;
        return p + 4;
    }
    return _number(p, v);
}

// Each top-level member of the object in text, once all of it is valid.
// Whatever follows the object is ignored. false: not a valid object.
inline bool object(const char* text, Fn fn, void* ctx) {
    const char* p = _ws(text);
    if (*p != '{' || !_members(p, nullptr, nullptr, 1)) return false;
    _members(p, fn, ctx, 1);
    return true;
}

// The members of an OBJECT value
inline void members(const Value& obj, Fn fn, void* ctx) { _members(obj.at, fn, ctx, 1); }

// Element 0 of an ARRAY value; BAD if the array is empty
inline Value first(const Value& array) {
    Value v;
    _items(array.at, 1, &v);
    return v;
}

// The member's name is exactly name (in flash)
inline bool is(const char* key, size_t len, PGM_P name) {
    return strlen_P(name) == len && memcmp_P(key, name, len) == 0;
}

// One decoded character of a valid string, as UTF-8 in out; advances p
inline size_t _decode(const char*& p, char out[4]) {
    char c = *p++;
    if (c != '\\') {
        out[0] = c;
        return 1;
    }
    c = *p++;
    if (c != 'u') {
        static const char MAP[] = "\"\"\\\\//b\bf\fn\nr\rt\t";
        out[0] = strchr(MAP, c)[1];
        return 1;
    }
    uint32_t cp = 0;
    for (int i = 0; i < 4; i++) cp = cp << 4 | (uint32_t)_hex(*p++);
    if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {   // surrogate pair
        uint32_t lo = 0;
        for (int i = 2; i < 6; i++) lo = lo << 4 | (uint32_t)_hex(p[i]);
        if (lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
        }
    }
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    size_t n = cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    static const uint8_t LEAD[] = {0, 0, 0xC0, 0xE0, 0xF0};
    for (size_t i = n - 1; i > 0; i--, cp >>= 6) out[i] = (char)(0x80 | (cp & 0x3F));
    out[0] = (char)(LEAD[n] | cp);
    return n;
}

// A STRING decoded into dst, cut to cap - 1 bytes and always terminated
// (cap > 0). Returns the whole decoded length: >= cap means it was cut.
inline size_t copy(const Value& s, char* dst, size_t cap) {
    size_t n = 0;
    for (const char* p = s.at; *p != '"';) {
        char c[4];
        size_t k = _decode(p, c);
        for (size_t i = 0; i < k; i++, n++)
            if (n + 1 < cap) dst[n] = c[i];
    }
    dst[n < cap ? n : cap - 1] = '\0';
    return n;
}

// A STRING decoded whole
inline String str(const Value& s) {
    String out;
    for (const char* p = s.at; *p != '"';) {
        char c[4];
        size_t k = _decode(p, c);
        for (size_t i = 0; i < k; i++) out += c[i];
    }
    return out;
}

// ── Writing ──────────────────────────────────────────────────────────────────

// A flat object, written to the file or only measured (file null). Keys in
// flash, up to 23 characters.
class Writer {
public:
    explicit Writer(File* f = nullptr) : _f(f) { _put("{", 1); }

    void str(PGM_P key, const char* s) {
        _key(key);
        _put("\"", 1);
        for (; *s; s++) {
            if (*s == '"' || *s == '\\') {
                _put("\\", 1);
                _put(s, 1);
            } else if ((uint8_t)*s < 0x20) {
                char u[8];
                snprintf(u, sizeof(u), "\\u%04x", (unsigned)(uint8_t)*s);
                _put(u, 6);
            } else {
                _put(s, 1);
            }
        }
        _put("\"", 1);
    }

    void num(PGM_P key, long v) {
        char b[12];
        snprintf(b, sizeof(b), "%ld", v);
        _key(key);
        _put(b, strlen(b));
    }

    void unum(PGM_P key, uint32_t v) {
        char b[12];
        snprintf(b, sizeof(b), "%lu", (unsigned long)v);
        _key(key);
        _put(b, strlen(b));
    }

    void boolean(PGM_P key, bool v) {
        _key(key);
        if (v) _put("true", 4);
        else _put("false", 5);
    }

    // Closes the object; the bytes written (or that would be)
    size_t end() {
        _put("}", 1);
        return _n;
    }

private:
    File*  _f;
    size_t _n = 0;
    bool   _first = true;

    void _key(PGM_P key) {
        char k[24];
        strncpy_P(k, key, sizeof(k) - 1);
        k[sizeof(k) - 1] = '\0';
        if (!_first) _put(",", 1);
        _first = false;
        _put("\"", 1);
        _put(k, strlen(k));
        _put("\":", 2);
    }

    void _put(const char* s, size_t n) {
        _n += n;
        if (_f) _f->write(reinterpret_cast<const uint8_t*>(s), n);
    }
};

}  // namespace json
