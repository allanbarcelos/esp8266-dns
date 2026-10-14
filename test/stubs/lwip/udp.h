// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
// Simulated raw lwIP UDP: keeps what was sent and lets the test
// deliver the answer through the registered callback.
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <string>

typedef uint16_t u16_t;
struct ip_addr_t { uint32_t addr; };
#define ip_addr_set_ip4_u32(ip, val) ((ip)->addr = (val))

enum pbuf_layer { PBUF_TRANSPORT };
enum pbuf_type  { PBUF_RAM };
struct pbuf {
    void* payload;
    u16_t len;
    u16_t tot_len;
};

struct udp_pcb;
typedef void (*udp_recv_fn)(void* arg, udp_pcb* pcb, pbuf* p, const ip_addr_t* addr, u16_t port);
struct udp_pcb {
    udp_recv_fn recv = nullptr;
    void*       arg  = nullptr;
};

namespace MockUDP {
    inline bool        failNew   = false;
    inline bool        failAlloc = false;
    inline int         live      = 0;    // open pcbs
    inline int         pbufs     = 0;    // pbufs not freed
    inline udp_pcb*    last      = nullptr;
    inline std::string sent;
    inline uint32_t    to        = 0;
    inline u16_t       port      = 0;
    inline void reset() {
        failNew = failAlloc = false;
        live = pbufs = 0; last = nullptr; sent.clear(); to = 0; port = 0;
    }
}

inline udp_pcb* udp_new() {
    if (MockUDP::failNew) return nullptr;
    MockUDP::live++;
    return MockUDP::last = new udp_pcb();
}
inline void udp_remove(udp_pcb* pcb) {
    MockUDP::live--;
    if (MockUDP::last == pcb) MockUDP::last = nullptr;
    delete pcb;
}
inline void udp_recv(udp_pcb* pcb, udp_recv_fn fn, void* arg) { pcb->recv = fn; pcb->arg = arg; }
inline int udp_sendto(udp_pcb*, pbuf* p, const ip_addr_t* to, u16_t port) {
    MockUDP::sent.assign(static_cast<const char*>(p->payload), p->len);
    MockUDP::to = to->addr;
    MockUDP::port = port;
    return 0;
}

inline pbuf* pbuf_alloc(pbuf_layer, u16_t len, pbuf_type) {
    if (MockUDP::failAlloc) return nullptr;
    MockUDP::pbufs++;
    pbuf* p = new pbuf{std::malloc(len ? len : 1), len, len};
    return p;
}
inline void pbuf_free(pbuf* p) {
    MockUDP::pbufs--;
    std::free(p->payload);
    delete p;
}

namespace MockUDP {
    // DNS server answer arriving at the open pcb; tot_len larger than
    // len simulates an answer in several pbufs
    inline void deliver(const std::string& data, u16_t extra = 0) {
        pbuf* p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)data.size(), PBUF_RAM);
        std::copy(data.begin(), data.end(), static_cast<char*>(p->payload));
        p->tot_len = (u16_t)(p->len + extra);
        last->recv(last->arg, last, p, nullptr, 53);
    }
}
