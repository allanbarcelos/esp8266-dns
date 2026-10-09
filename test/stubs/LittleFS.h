#pragma once
#include "Arduino.h"
#include <map>
#include <string>
#include <vector>
#include <algorithm>

// ── FS in-memory ─────────────────────────────────────────────────────────────
namespace MockFS {
    inline std::map<std::string, std::string> files;
    inline bool beginOk       = true;
    inline bool failReadOpen  = false;
    inline bool failWriteOpen = false;
    inline bool failRename    = false;

    inline void reset() {
        files.clear();
        beginOk = true; failReadOpen = false; failWriteOpen = false; failRename = false;
    }
    inline void put(const char* path, const std::string& content) { files[path] = content; }
}

struct FSInfo {
    size_t totalBytes = 65536;
    size_t usedBytes  = 8192;
};

// ── Arquivo mock com interface de leitura (Stream) e escrita (Print) ──────────
class File {
public:
    File() : _valid(false) {}
    File(const std::string& data, bool writing, std::string path)
        : _data(data), _pos(0), _writing(writing), _path(std::move(path)), _valid(true) {}

    explicit operator bool() const { return _valid; }

    // Leitura (deserializeJson)
    int read() { return (!_writing && _pos < _data.size()) ? (uint8_t)_data[_pos++] : -1; }
    size_t readBytes(char* buf, size_t n) {
        size_t k = std::min(n, _data.size() - _pos);
        std::memcpy(buf, _data.data() + _pos, k);
        _pos += k;
        return k;
    }

    // Escrita (serializeJson)
    size_t write(uint8_t c)                  { _data += (char)c; return 1; }
    size_t write(const uint8_t* b, size_t n) { _data.append((const char*)b, n); return n; }

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
    bool begin() { return MockFS::beginOk; }

    bool exists(const char* path) { return MockFS::files.count(path) > 0; }

    File open(const char* path, const char* mode) {
        if (mode[0] == 'r') {
            if (MockFS::failReadOpen || !exists(path)) return File();
            return File(MockFS::files[path], false, path);
        }
        if (MockFS::failWriteOpen) return File();
        return File("", true, path);
    }

    bool rename(const char* from, const char* to) {
        if (MockFS::failRename || !exists(from)) return false;
        MockFS::files[to] = MockFS::files[from];
        MockFS::files.erase(from);
        return true;
    }

    bool remove(const char* path) { return MockFS::files.erase(path) > 0; }

    bool info(FSInfo& i) { i = FSInfo(); return true; }
    Dir  openDir(const char*) { return Dir(); }
};

inline LittleFSClass LittleFS;
