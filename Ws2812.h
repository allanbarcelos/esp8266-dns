// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// WS2812 (NeoPixel) strip with a single color: no library and no per-LED
// buffer. Each bit lasts 1.25 µs (0: 0.4 µs high; 1: 0.8 µs high), in G, R,
// B order. Interrupts stay off while sending (30 µs per LED; 60 LEDs take
// 1.8 ms), otherwise Wi-Fi stretches a bit and the color comes out wrong.
// After sending, the pin held low for more than 50 µs latches the color.
#if defined(ARDUINO_ARCH_ESP8266)
#include <esp8266_peri.h>

// Must be inline: a call into flash inside the loop would delay the bit
static inline __attribute__((always_inline)) uint32_t _ws2812Cycles() {
    uint32_t c;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
    return c;
}

static void IRAM_ATTR ws2812Show(uint8_t pin, uint16_t count, uint32_t rgb) {
    const uint32_t mask = 1u << pin;
    const uint32_t t0 = F_CPU / 2500000;   // 0,4 µs
    const uint32_t t1 = F_CPU / 1250000;   // 0,8 µs
    const uint32_t period = F_CPU / 800000; // 1,25 µs
    const uint8_t grb[3] = {(uint8_t)(rgb >> 8), (uint8_t)(rgb >> 16), (uint8_t)rgb};

    noInterrupts();
    uint32_t start = _ws2812Cycles() - period;
    for (uint16_t led = 0; led < count; led++) {
        for (uint8_t i = 0; i < 3; i++) {
            for (uint8_t bit = 0x80; bit; bit >>= 1) {
                uint32_t high = (grb[i] & bit) ? t1 : t0;
                uint32_t c;
                while (((c = _ws2812Cycles()) - start) < period) {}
                GPOS = mask;
                start = c;
                while ((_ws2812Cycles() - start) < high) {}
                GPOC = mask;
            }
        }
    }
    interrupts();
}
#else
// Native tests: keeps the last send
namespace mock {
    inline int      wsPin = -1;
    inline uint16_t wsCount = 0;
    inline uint32_t wsColor = 0;
    inline int      wsShows = 0;
}
inline void ws2812Show(uint8_t pin, uint16_t count, uint32_t rgb) {
    mock::wsPin = pin;
    mock::wsCount = count;
    mock::wsColor = rgb;
    mock::wsShows++;
}
#endif
