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
// for the panel and the log, and where the loop is: if it crashes or the
// software watchdog fires, the next boot logs the place.
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

    void at(uint8_t where) { _at = where; }

    // From the crash handler (an exception or the software watchdog): where
    // the loop was and the first code address on the stack go to RTC
    // memory, which survives the reboot
    void crashed(const uint32_t* sp, const uint32_t* end) const {
        uint32_t c[3] = {CRUMB, _at, 0};
        for (int i = 0; sp < end && i < 64 && !c[2]; sp++, i++)
            if (*sp >= 0x40200000u && *sp < 0x40300000u) c[2] = *sp;
        ESP.rtcUserMemoryWrite(RTC_SLOT, c, sizeof(c));
    }

    // "ip near 0x40212345" once, if the previous boot crashed (out: CRASH_LEN)
    static constexpr size_t CRASH_LEN = 40;
    bool lastCrash(char* out) {
        uint32_t c[3] = {};
        ESP.rtcUserMemoryRead(RTC_SLOT, c, sizeof(c));
        if (c[0] != CRUMB || c[1] > MQTT) return false;
        snprintf(out, CRASH_LEN, "%s near 0x%08x", name((uint8_t)c[1]), (unsigned)c[2]);
        c[0] = 0;
        ESP.rtcUserMemoryWrite(RTC_SLOT, c, sizeof(c));
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
    static constexpr uint32_t CRUMB    = 0x4352554Du;   // "CRUM": the slot holds a crash
    static constexpr uint32_t RTC_SLOT = 120;           // 4-byte blocks; the start is eboot's

    uint8_t       _wants = 0, _queue = 0, _at = NONE;
    Who           _owner = NONE, _worstWho = NONE;
    bool          _starved = false;
    unsigned long _waitFrom = 0, _worst = 0;
    uint16_t      _slow = 0, _late = 0;
};
