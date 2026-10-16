// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// Who has the network: the ESP-01 fits one TLS session at a time, and the
// services that need one queue here by priority. Every tick, each service
// that would use the network says so (want); grant() gives the turn to the
// most important one, and App closes everyone else's session before it runs.
//
// A turn starts only once the heap fits it (each service needs a different
// amount: the GitHub session takes a 16 KB buffer). A session just closed
// may hold memory a few seconds more (lwIP keeps the socket in TIME_WAIT),
// so the turn waits for the heap instead of failing, up to WAIT_MS; then
// the service runs anyway and its own fallback applies (OTA restarts into
// recovery). While a turn waits, App also pauses MQTT, whose buffers are
// heap too.
//
// It also keeps the slowest loop seen (a TLS handshake blocks for seconds),
// for the panel and the log, and where the loop is: if it crashes or a
// watchdog fires, the next boot logs the place.
class NetArbiter {
public:
    // In priority order
    enum Who : uint8_t { OTA, IP, DNS, SETUP, POST, RELAY, STREAM, COUNT, NONE = COUNT };
    // Where the loop is: a Who while that service runs, or one of these
    enum At : uint8_t { WEB = NONE + 1, WIFI, IOT, MQTT };

    static constexpr unsigned long WAIT_MS  = 30000UL;   // longest wait for the heap
    static constexpr unsigned long SLOW_MS  = 200UL;     // a loop this long is a stall

    // The heap a turn needs: total, and the largest block
    struct Need {
        uint32_t heap;
        uint32_t block;
    };

    void want(Who w) { _wants |= (uint8_t)(1u << w); }

    // The turn for this tick (NONE if nobody wants it); the wants start over
    Who grant() {
        Who w = NONE;
        for (uint8_t i = 0; i < COUNT && w == NONE; i++)
            if (_wants >> i & 1) w = (Who)i;
        _queue = _wants;
        _wants = 0;
        if (w != _owner) {
            _owner = w;
            _starved = false;
        }
        return w;
    }

    Who owner() const { return _owner; }

    // The owner may go on: its session is already open (running), the heap
    // fits a new one, or it has waited WAIT_MS for it
    bool ready(unsigned long now, const Need& need, bool running = false) {
        if (running || (ESP.getFreeHeap() >= need.heap && ESP.getMaxFreeBlockSize() >= need.block)) {
            _starved = false;
            return true;
        }
        if (!_starved) {
            _starved = true;
            _waitFrom = now;
        }
        if (now - _waitFrom < WAIT_MS) return false;
        _starved = false;   // still short next tick: another wait
        _late++;
        return true;
    }

    // Waiting for the heap: other users of it (MQTT) let go
    bool starved() const { return _starved; }

    // One loop took ms
    void loop(unsigned long ms) {
        if (ms < SLOW_MS) return;
        if (_slow < 0xFFFF) _slow++;
        if (ms > _worst) {
            _worst = ms;
            _worstWho = _owner;
        }
    }

    unsigned long worst() const    { return _worst; }
    uint16_t      slow() const     { return _slow; }
    uint16_t      late() const     { return _late; }   // turns that ran without the heap
    Who           worstWho() const { return _worstWho; }

    static const char* name(uint8_t w) {
        static const char* const NAMES[] = {"ota", "ip", "dns", "cloudflare", "ntfy post", "relay",
                                            "ntfy stream", "-", "web", "wifi", "iot", "mqtt"};
        return NAMES[w];
    }

    // Each part of the loop says so as it starts; RTC memory keeps it
    // through a reset, so after a crash or a watchdog the next boot knows
    // where the loop was. (The core's crash callback does not run on every
    // watchdog reset.)
    void at(uint8_t where) {
        if (where == _at) return;
        _at = where;
        uint32_t v = CRUMB | where;
        ESP.rtcUserMemoryWrite(RTC_SLOT, &v, sizeof(v));
    }

    // "ip near 0x40212345" if the last reset was a crash or a watchdog: the
    // part of the loop, and the address the chip recorded (out: CRASH_LEN)
    static constexpr size_t CRASH_LEN = 40;
    static bool lastCrash(char* out) {
        const rst_info* r = ESP.getResetInfoPtr();
        uint32_t v = 0;
        ESP.rtcUserMemoryRead(RTC_SLOT, &v, sizeof(v));
        if (r->reason < REASON_WDT_RST || r->reason > REASON_SOFT_WDT_RST ||
            (v & ~0xFFu) != CRUMB || (v & 0xFFu) > MQTT)
            return false;
        snprintf(out, CRASH_LEN, "%s near 0x%08x", name((uint8_t)v), (unsigned)r->epc1);
        return true;
    }

    // "dns, waiting: relay, ntfy stream" for the panel (out: QUEUE_LEN)
    static constexpr size_t QUEUE_LEN = 96;
    const char* queue(char* out) const {
        size_t n = (size_t)snprintf(out, QUEUE_LEN, "%s%s", name(_owner), _starved ? " (memory)" : "");
        const char* sep = ", waiting: ";
        for (uint8_t i = 0; i < COUNT; i++) {
            if (!(_queue >> i & 1) || i == _owner) continue;
            n += (size_t)snprintf(out + n, QUEUE_LEN - n, "%s%s", sep, name((Who)i));
            sep = ", ";
        }
        return out;
    }

private:
    static constexpr uint32_t CRUMB    = 0x43525500u;   // "CRU" and the part of the loop
    static constexpr uint32_t RTC_SLOT = 40;            // 4-byte blocks; eboot's command takes 0-31

    uint8_t       _wants = 0, _queue = 0, _at = 0xFF;   // nothing recorded yet
    Who           _owner = NONE, _worstWho = NONE;
    bool          _starved = false;
    unsigned long _waitFrom = 0, _worst = 0;
    uint16_t      _slow = 0, _late = 0;
};
