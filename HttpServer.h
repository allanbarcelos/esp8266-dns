// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>

// Servidor HTTP do painel, no lugar do ESP8266WebServer do core: só o que
// as rotas daqui usam. Um cliente por vez; a resposta termina fechando a
// conexão (sem keep-alive nem chunked). Sem upload, sem Digest, sem tabela
// de rotas (o WebService decide pelo caminho) e sem std::function.
//
// Mesma semântica do core onde importa: argumentos da query e do corpo
// urlencoded (o primeiro com o nome vale), e o corpo cru em "plain"
// (curl -d ligar). Uma conexão sem pedido espera até WAIT_MS; se outra
// chega com dados, a parada cede depois de YIELD_MS (pré-conexão do
// navegador).
class HttpServer {
public:
    enum : uint8_t { GET, POST, OTHER };
    using Handler = void (*)(void* ctx);

    static constexpr size_t        UNKNOWN_LENGTH = (size_t)-1;
    static constexpr size_t        MAX_BODY       = 2048;
    static constexpr size_t        MAX_AUTH       = 200;
    static constexpr unsigned long WAIT_MS        = 5000;   // pedido depois de conectar
    static constexpr unsigned long READ_MS        = 2000;   // cada pedaço do pedido
    static constexpr unsigned long YIELD_MS       = 30;

    explicit HttpServer(uint16_t port) : _server(port) {}

    void begin(Handler h, void* ctx) {
        _handler = h;
        _ctx     = ctx;
        _server.begin();
    }

    void handleClient() {
        if (!_client.connected()) {          // nenhuma, ou a anterior fechou
            _client.stop();
            _client = _server.accept();
            if (!_client) return;
            _since = millis();
        }
        if (!_client.available()) {
            unsigned long idle = millis() - _since;
            bool other = _server.hasClientData() || _server.hasMaxPendingClients();
            if (idle >= WAIT_MS || (other && idle > YIELD_MS)) _client.stop();
            return;
        }
        _serve();
        _client.stop();
    }

    // ── Pedido ───────────────────────────────────────────────────────────────
    uint8_t       method() const        { return _method; }
    const char*   path() const          { return _path; }
    const String& authorization() const { return _auth; }

    // Valor do argumento (query, depois corpo urlencoded); "plain" é o corpo
    String arg(const char* name) const {
        if (strcmp(name, "plain") == 0) return _body;
        String v;
        if (_find(_query, name, v) || (_form && _find(_body, name, v))) return v;
        return String();
    }

    // ── Resposta ─────────────────────────────────────────────────────────────
    void sendHeader(const char* name, const String& value) {
        _headers += name;
        _headers += ": ";
        _headers += value;
        _headers += "\r\n";
    }

    void setContentLength(size_t len) { _length = len; }

    void send(int code, const char* type, const String& body) {
        if (_length != UNKNOWN_LENGTH) _length = body.length();
        _head(code, type);
        _put(body.c_str(), body.length(), false);
    }

    // Corpo na flash (imagens, CSS e JS embutidos)
    void send_P(int code, const char* type, PGM_P data, size_t len) {
        _length = len;
        _head(code, type);
        _put(data, len, true);
    }

    void sendContent(const char* s)   { _put(s, strlen(s), false); }
    void sendContent(const String& s) { _put(s.c_str(), s.length(), false); }
    void sendContent_P(PGM_P s)       { _put(s, strlen_P(s), true); }

private:
    WiFiServer    _server;
    WiFiClient    _client;
    Handler       _handler = nullptr;
    void*         _ctx     = nullptr;
    unsigned long _since   = 0;

    uint8_t _method = OTHER;
    char    _path[24] = {};
    String  _query, _body, _auth, _headers;
    bool    _form   = false;
    size_t  _length = 0;

    char    _out[256] = {};
    size_t  _used = 0;

    void _serve() {
        _path[0] = '\0';
        _query = String();
        _body = String();
        _auth = String();
        _headers = String();
        _form = false;
        _length = 0;
        _used = 0;

        int code = _parse();
        if (code) {
            send(code, "text/plain", "");
        } else {
            _handler(_ctx);
        }
        _flush();
    }

    // 0 = pedido pronto para o handler; senão o código do erro
    int _parse() {
        char line[MAX_AUTH + 24];
        if (!_line(line, sizeof(line))) return 400;
        char* sp = strchr(line, ' ');
        char* end = sp ? strchr(sp + 1, ' ') : nullptr;
        if (!end) return 400;
        *sp = *end = '\0';
        _method = strcmp(line, "GET") == 0 ? GET : strcmp(line, "POST") == 0 ? POST : OTHER;
        char* q = strchr(sp + 1, '?');
        if (q) {
            *q = '\0';
            _query = q + 1;
        }
        if (strlen(sp + 1) >= sizeof(_path)) return 404;   // nenhuma rota é tão longa
        strcpy(_path, sp + 1);

        long length = 0;
        for (;;) {
            if (!_line(line, sizeof(line))) return 400;
            if (!line[0]) break;
            char* v = strchr(line, ':');
            if (!v) continue;
            *v++ = '\0';
            while (*v == ' ') v++;
            if (strcasecmp(line, "Authorization") == 0) _auth = v;
            else if (strcasecmp(line, "Content-Length") == 0) length = atol(v);
            else if (strcasecmp(line, "Content-Type") == 0)
                _form = strstr(v, "x-www-form-urlencoded") != nullptr;
        }
        if (length < 0 || (size_t)length > MAX_BODY) return 413;
        _body.reserve(length);
        while (length--) {
            int c = _getc();
            if (c < 0) return 400;
            _body += (char)c;
        }
        return 0;
    }

    // Uma linha sem o \r\n; o que passar do buffer é descartado
    bool _line(char* buf, size_t cap) {
        size_t n = 0;
        for (;;) {
            int c = _getc();
            if (c < 0) return false;
            if (c == '\n') break;
            if (c != '\r' && n + 1 < cap) buf[n++] = (char)c;
        }
        buf[n] = '\0';
        return true;
    }

    int _getc() {
        unsigned long t = millis();
        while (!_client.available()) {
            if (!_client.connected() || millis() - t >= READ_MS) return -1;
            delay(1);
        }
        return _client.read();
    }

    // Procura name=valor em a=1&b=2 e decodifica (+ e %XX)
    static bool _find(const String& s, const char* name, String& out) {
        const char* p = s.c_str();
        size_t n = strlen(name);
        while (*p) {
            const char* amp = strchr(p, '&');
            const char* stop = amp ? amp : p + strlen(p);
            if ((size_t)(stop - p) >= n && strncmp(p, name, n) == 0 && (p[n] == '=' || p + n == stop)) {
                const char* v = p + n + (p + n < stop);
                out = String();
                while (v < stop) {
                    char c = *v++;
                    if (c == '+') {
                        c = ' ';
                    } else if (c == '%' && stop - v >= 2 && isxdigit((unsigned char)v[0]) &&
                               isxdigit((unsigned char)v[1])) {
                        const char hex[3] = {v[0], v[1], '\0'};
                        c = (char)strtol(hex, nullptr, 16);
                        v += 2;
                    }
                    out += c;
                }
                return true;
            }
            p = amp ? amp + 1 : stop;
        }
        return false;
    }

    void _head(int code, const char* type) {
        char line[96];
        snprintf_P(line, sizeof(line), PSTR("HTTP/1.1 %d \r\nContent-Type: %s\r\n"), code, type);
        _put(line, strlen(line), false);
        if (_length != UNKNOWN_LENGTH) {
            snprintf_P(line, sizeof(line), PSTR("Content-Length: %u\r\n"), (unsigned)_length);
            _put(line, strlen(line), false);
        }
        _put(_headers.c_str(), _headers.length(), false);
        sendContent_P(PSTR("Connection: close\r\n\r\n"));
    }

    void _put(const char* p, size_t n, bool pgm) {
        while (n) {
            if (_used == sizeof(_out)) _flush();
            size_t k = sizeof(_out) - _used;
            if (k > n) k = n;
            if (pgm) memcpy_P(_out + _used, p, k);
            else memcpy(_out + _used, p, k);
            _used += k;
            p += k;
            n -= k;
        }
    }

    // Só é chamado com algo no buffer: cheio, ou no fim da resposta
    void _flush() {
        _client.write(reinterpret_cast<const uint8_t*>(_out), _used);
        _used = 0;
    }
};
