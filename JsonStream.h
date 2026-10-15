// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// JSON read one byte at a time, for answers too big to keep (a Cloudflare
// zone list runs to kilobytes while TLS holds most of the heap). Each
// scalar is handed to a callback with its place: depth() and the key at
// each level (key(1) is the top-level member). The end of an object comes
// as a call with value == nullptr, at the object's own depth.
//
// Not a validator: junk gives odd calls, never a fault. Text longer than
// VALUE_LEN is cut (cut() says so); escapes keep the escaped character,
// and \uXXXX becomes '?'. Deeper than MAX_DEPTH, nothing is reported.
class JsonStream {
public:
    static constexpr uint8_t MAX_DEPTH = 5;
    static constexpr size_t  KEY_LEN   = 12;
    static constexpr size_t  VALUE_LEN = 72;

    using Fn = void (*)(void* ctx, const JsonStream& s, const char* value);

    JsonStream(Fn fn, void* ctx) : _fn(fn), _ctx(ctx) {}

    uint8_t depth() const { return _depth; }
    bool    cut() const   { return _cut; }

    // Key at a level (1 = top); "" inside an array or out of range
    const char* key(uint8_t level) const {
        return level >= 1 && level <= MAX_DEPTH ? _keys[level - 1] : "";
    }

    bool is(uint8_t level, PGM_P k) const { return strcmp_P(key(level), k) == 0; }

    void feed(char c) {
        if (_str) {
            _string(c);
            return;
        }
        switch (c) {
            case '"':
                _scalar();
                _str = true;
                _n = 0;
                _cut = false;
                break;
            case '{':
            case '[':
                _scalar();
                _open(c == '{');
                break;
            case '}':
            case ']':
                _scalar();
                if (c == '}' && _depth >= 1 && _depth <= MAX_DEPTH) _fn(_ctx, *this, nullptr);
                if (_depth) _depth--;
                break;
            case ',':
                _scalar();
                _wantKey = _inObject();
                break;
            case ':':
                _wantKey = false;
                break;
            case ' ': case '\t': case '\r': case '\n':
                _scalar();
                break;
            default:
                if (!_n) _cut = false;
                _put(c);
                _bare = true;
        }
    }

private:
    Fn       _fn;
    void*    _ctx;
    char     _keys[MAX_DEPTH][KEY_LEN] = {};
    uint8_t  _objects = 0;      // bit i: level i + 1 is an object
    uint8_t  _depth = 0;
    bool     _str = false, _esc = false, _bare = false, _wantKey = false, _cut = false;
    uint8_t  _hex = 0;          // \u digits still to skip
    char     _buf[VALUE_LEN] = {};
    size_t   _n = 0;

    bool _inObject() const { return _depth >= 1 && _depth <= 8 && (_objects >> (_depth - 1) & 1); }

    void _open(bool object) {
        if (_depth == 255) return;   // junk: stays put rather than wrap
        _depth++;
        if (_depth <= 8) {
            uint8_t bit = (uint8_t)(1u << (_depth - 1));
            _objects = object ? (_objects | bit) : (_objects & ~bit);
        }
        if (_depth <= MAX_DEPTH) _keys[_depth - 1][0] = '\0';
        _wantKey = object;
    }

    void _put(char c) {
        if (_n + 1 < VALUE_LEN) _buf[_n++] = c;
        else _cut = true;
    }

    void _string(char c) {
        if (_hex) {
            _hex--;
            return;
        }
        if (_esc) {
            _esc = false;
            if (c == 'u') {
                _hex = 4;
                c = '?';
            }
            _put(c);
            return;
        }
        if (c == '\\') {
            _esc = true;
            return;
        }
        if (c != '"') {
            _put(c);
            return;
        }
        _str = false;
        _buf[_n] = '\0';
        if (_wantKey) {
            if (_depth >= 1 && _depth <= MAX_DEPTH) strlcpy(_keys[_depth - 1], _buf, KEY_LEN);
            _wantKey = false;
        } else {
            _emit();
        }
        _n = 0;
    }

    // A number or literal ends
    void _scalar() {
        if (!_bare) return;
        _bare = false;
        _buf[_n] = '\0';
        _emit();
        _n = 0;
    }

    void _emit() {
        if (_depth >= 1 && _depth <= MAX_DEPTH) _fn(_ctx, *this, _buf);
    }
};
