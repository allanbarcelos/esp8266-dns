#pragma once
#include "Arduino.h"

class UpdaterClass {
public:
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
    // Mesma semântica do core: incompleto e !evenIfRemaining → aborta
    bool end(bool evenIfRemaining = false) {
        if (_written < _size && !evenIfRemaining) { _aborts++; return false; }
        if (evenIfRemaining) _size = _written;
        _ends++;
        return _endOk;
    }
    String getErrorString() { return String("Signature Verification Failed"); }

    // Controláveis nos testes
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
