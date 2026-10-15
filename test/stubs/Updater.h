// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "Arduino.h"

// The core's interfaces for the image hash and the signature check
class UpdaterHashClass {
public:
    virtual ~UpdaterHashClass() {}
    virtual void begin() = 0;
    virtual void add(const void* data, uint32_t len) = 0;
    virtual void end() = 0;
    virtual int len() = 0;
    virtual const void* hash() = 0;
    virtual const unsigned char* oid() = 0;
};

class UpdaterVerifyClass {
public:
    virtual ~UpdaterVerifyClass() {}
    virtual uint32_t length() = 0;
    virtual bool verify(UpdaterHashClass* hash, const void* signature, uint32_t len) = 0;
};

class UpdaterClass {
public:
    bool installSignature(UpdaterHashClass* hash, UpdaterVerifyClass* verify) {
        _hash = hash;
        _verify = verify;
        return true;
    }
    UpdaterHashClass*   _hash   = nullptr;
    UpdaterVerifyClass* _verify = nullptr;

    bool begin(size_t size) {
        if (size == 0) return false;
        _size = size; _written = 0;
        return _beginOk;
    }
    size_t write(uint8_t*, size_t n) {
        size_t w = _shortWrite ? n / 2 : n;
        _written += w;
        return w;
    }
    // Same semantics as the core: incomplete and !evenIfRemaining → aborts
    bool end(bool evenIfRemaining = false) {
        if (_written < _size && !evenIfRemaining) { _aborts++; return false; }
        if (evenIfRemaining) _size = _written;
        _ends++;
        return _endOk;
    }
    String getErrorString() { return String("Signature Verification Failed"); }

    // Controlled by the tests
    bool   _beginOk    = true;
    bool   _endOk      = true;
    bool   _shortWrite = false;
    size_t _size       = 0;
    size_t _written    = 0;
    int    _aborts     = 0;
    int    _ends       = 0;

    void reset() { *this = UpdaterClass(); }
};

inline UpdaterClass Update;
