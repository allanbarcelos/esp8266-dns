#pragma once
#include "Arduino.h"
#include <map>
#include <string>
#include <vector>
#include <algorithm>

// ── FS in-memory ─────────────────────────────────────────────────────────────
namespace MockFS {
    inline std::map<std::string, std::string> files;
    inline void reset()                                    { files.clear(); }
    inline void put(const char* path, const std::string& content) {
        files[path] = content;
    }
}

struct FSInfo {
    size_t totalBytes = 1048576;
    size_t usedBytes  = 4096;
    size_t blockSize  = 4096;
    size_t pageSize   = 256;
    size_t maxOpenFiles = 5;
    size_t maxPathLength = 32;
};

// ── Arquivo mock com interface Stream (leitura) e Print (escrita) ─────────────
class File {
public:
    File() : _valid(false) {}
    File(const std::string& data, bool writing, std::string path)
        : _data(data), _pos(0), _writing(writing), _path(std::move(path)), _valid(true) {}

    operator bool() const { return _valid; }

    // Leitura (deserializeJson)
    int    available() { return _writing ? 0 : (int)(_data.size() - _pos); }
    int    read()      { return (!_writing && _pos < _data.size()) ? (uint8_t)_data[_pos++] : -1; }
    size_t readBytes(char* buf, size_t n) {
        size_t k = std::min(n, _data.size() - _pos);
        if (!_writing) { std::memcpy(buf, _data.data() + _pos, k); _pos += k; }
        return k;
    }

    // Escrita (serializeJson)
    size_t write(uint8_t c)                    { if (_writing) _data += (char)c; return 1; }
    size_t write(const uint8_t* b, size_t n)   {
        if (_writing) _data.append((const char*)b, n); return n;
    }
    size_t print(const char* s) {
        size_t n = std::strlen(s);
        if (_writing) _data += s;
        return n;
    }

    void close() {
        if (_valid && _writing) MockFS::files[_path] = _data;
        _valid = false;
    }

private:
    std::string _data;
    size_t      _pos     = 0;
    bool        _writing = false;
    std::string _path;
    bool        _valid   = false;
};

// ── Dir mock ──────────────────────────────────────────────────────────────────
class Dir {
    std::vector<std::string> _keys;
    size_t _idx = 0;
public:
    Dir() { for (auto& kv : MockFS::files) _keys.push_back(kv.first); }
    bool next() { return _idx < _keys.size() && (_idx++, true); }
};

// ── LittleFS mock ─────────────────────────────────────────────────────────────
class LittleFSClass {
public:
    bool begin() { return true; }
    bool end()   { return true; }

    bool exists(const char* path) { return MockFS::files.count(path) > 0; }

    File open(const char* path, const char* mode) {
        if (mode[0] == 'r') {
            if (!exists(path)) return File();
            return File(MockFS::files[path], false, path);
        }
        return File("", true, path);
    }

    bool info(FSInfo& i) { i = FSInfo(); return true; }
    Dir  openDir(const char*) { return Dir(); }
};

inline LittleFSClass LittleFS;
