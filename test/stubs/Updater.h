#pragma once
#include "Arduino.h"

class UpdaterClass {
public:
    bool   begin(size_t size)             { _size = size; _written = 0; return _ok; }
    size_t write(uint8_t*, size_t n)      { _written += n; return n; }
    bool   end(bool cancel = false)       { return !cancel && _ok; }
    String getErrorString()               { return String("mock error"); }
    bool   isRunning()                    { return false; }

    // Controláveis nos testes
    bool   _ok      = true;
    size_t _size    = 0;
    size_t _written = 0;
};

inline UpdaterClass Update;
