// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <ESP8266WiFi.h>
#include <lwip/udp.h>

// Nome reverso (PTR) de um IPv4, perguntado direto ao DNS da rede por UDP.
// O lwIP do core só resolve nome → IP; a pergunta e a leitura da resposta
// ficam aqui, sem biblioteca. A resposta chega depois, pelo callback do
// lwIP; sem resposta em TIMEOUT, o nome fica vazio.
class ReverseDNS {
public:
    static constexpr unsigned long TIMEOUT = 5000;

    ~ReverseDNS() { _close(); }

    // Pergunta o PTR de ip; out recebe o nome quando a resposta chegar.
    // out precisa viver até lá (é o Config).
    void lookup(const char* ip, char* out, size_t len, unsigned long now) {
        _close();
        out[0] = '\0';
        uint8_t q[48];
        size_t n = query(ip, ++_id, q, sizeof(q));
        IPAddress dns = WiFi.dnsIP();
        if (n == 0 || !dns.isSet()) return;
        pbuf* p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
        if (!p) return;
        _pcb = udp_new();
        if (_pcb) {
            _out = out;
            _len = len;
            _sentAt = now;
            udp_recv(_pcb, _onRecv, this);
            memcpy(p->payload, q, n);
            ip_addr_t to;
            ip_addr_set_ip4_u32(&to, (uint32_t)dns);
            udp_sendto(_pcb, p, &to, 53);
        }
        pbuf_free(p);
    }

    void tick(unsigned long now) {
        if (_pcb && now - _sentAt >= TIMEOUT) _close();
    }

    bool pending() const { return _pcb != nullptr; }

    // Pergunta "d.c.b.a.in-addr.arpa PTR" com recursão. 0 se ip inválido.
    static size_t query(const char* ip, uint16_t id, uint8_t* buf, size_t cap) {
        IPAddress a;
        if (cap < 46 || !a.fromString(ip)) return 0;
        static const uint8_t HEAD[] = {0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
        buf[0] = (uint8_t)(id >> 8);
        buf[1] = (uint8_t)id;
        memcpy(buf + 2, HEAD, sizeof(HEAD));
        size_t i = 12;
        for (int k = 3; k >= 0; k--) {
            int len = snprintf((char*)buf + i + 1, 4, "%u", a[k]);
            buf[i] = (uint8_t)len;
            i += 1 + len;
        }
        static const uint8_t TAIL[] = {7, 'i', 'n', '-', 'a', 'd', 'd', 'r',
                                       4, 'a', 'r', 'p', 'a', 0, 0, 12, 0, 1};
        memcpy(buf + i, TAIL, sizeof(TAIL));
        return i + sizeof(TAIL);
    }

    // Nome do primeiro PTR da resposta em out. false se não for a resposta
    // desta pergunta, se der erro ou se não houver PTR.
    static bool parse(const uint8_t* m, size_t n, uint16_t id, char* out, size_t len) {
        if (n < 12 || _u16(m) != id || !(m[2] & 0x80) || (m[3] & 0x0F)) return false;
        size_t i = 12;
        for (uint16_t q = _u16(m + 4); q; q--) i = _skip(m, n, i) + 4;
        for (uint16_t a = _u16(m + 6); a; a--) {
            i = _skip(m, n, i);
            if (i + 10 > n) return false;
            uint16_t type = _u16(m + i), rd = _u16(m + i + 8);
            i += 10;
            if (i + rd > n) return false;
            if (type == 12) return _name(m, n, i, out, len);
            i += rd;
        }
        return false;
    }

private:
    udp_pcb*      _pcb = nullptr;
    char*         _out = nullptr;
    size_t        _len = 0;
    unsigned long _sentAt = 0;
    uint16_t      _id = 0;

    void _close() {
        if (_pcb) udp_remove(_pcb);
        _pcb = nullptr;
    }

    // Resposta numa pbuf só (a de um PTR cabe); qualquer resposta encerra
    static void _onRecv(void* arg, udp_pcb*, pbuf* p, const ip_addr_t*, u16_t) {
        ReverseDNS* self = static_cast<ReverseDNS*>(arg);
        if (p->len == p->tot_len)
            parse(static_cast<const uint8_t*>(p->payload), p->len, self->_id,
                  self->_out, self->_len);
        pbuf_free(p);
        self->_close();
    }

    static uint16_t _u16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

    // Posição depois de um nome (rótulos ou ponteiro). Fora da mensagem: n + 1
    static size_t _skip(const uint8_t* m, size_t n, size_t i) {
        while (i < n) {
            uint8_t c = m[i];
            if (c == 0) return i + 1;
            if ((c & 0xC0) == 0xC0) return i + 2;
            i += 1 + c;
        }
        return n + 1;
    }

    // Nome com rótulos separados por ponto; segue ponteiros de compressão
    // (no máximo 8, contra laço) e nunca passa do fim de out.
    static bool _name(const uint8_t* m, size_t n, size_t i, char* out, size_t len) {
        size_t o = 0;
        for (int jumps = 0; i < n;) {
            uint8_t c = m[i];
            if (c == 0) {
                out[o] = '\0';
                return o > 0;
            }
            if ((c & 0xC0) == 0xC0) {
                if (i + 1 >= n || ++jumps > 8) break;
                i = (size_t)(c & 0x3F) << 8 | m[i + 1];
                continue;
            }
            if (i + 1 + c > n || o + c + 2 > len) break;
            if (o) out[o++] = '.';
            memcpy(out + o, m + i + 1, c);
            o += c;
            i += 1 + c;
        }
        out[0] = '\0';
        return false;
    }
};
