// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>

// Fita WS2812 (NeoPixel) com uma cor só: dispensa biblioteca e buffer por
// LED. Cada bit dura 1,25 µs (0: 0,4 µs alto; 1: 0,8 µs alto), na ordem
// G, R, B. A interrupção fica fechada durante o envio (30 µs por LED; 60
// LEDs dão 1,8 ms), senão o Wi-Fi estica um bit e a cor sai errada.
// Depois do envio, o pino em nível baixo por mais de 50 µs fixa a cor.
#if defined(ARDUINO_ARCH_ESP8266)
#include <esp8266_peri.h>

// Inline obrigatório: chamada para a flash dentro do laço atrasaria o bit
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
// Testes nativos: guarda o último envio
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
