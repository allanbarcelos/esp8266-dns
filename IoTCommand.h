// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <Arduino.h>
#include "IoTCatalog.h"

// Comandos de texto, iguais no painel, na API local e no ntfy:
//   ligar | desligar | alternar | pulso 5[s] | 50% | nivel 50 | brilho 50
//   angulo 90 | abrir | fechar | cor #ff8800 | auto | status
// Maiúsculas e minúsculas tanto faz; há sinônimos em inglês (on, off,
// toggle, pulse, level, angle, color, open, close). "abrir" e "fechar"
// são ligar e desligar: o servo vai para o ângulo aberto ou fechado.
namespace iot {

enum Op : uint8_t {
    OP_NONE, OP_ON, OP_OFF, OP_TOGGLE, OP_PULSE, OP_LEVEL, OP_ANGLE, OP_COLOR,
    OP_AUTO, OP_STATUS
};

struct Command {
    uint8_t  op    = OP_NONE;
    uint32_t value = 0;   // segundos, %, graus ou 0xRRGGBB
};

// Na flash: a tabela inteira ficaria ~400 bytes na RAM que o TLS usa
struct _Word { char word[12]; uint8_t op; };

// UTF-8 nos acentos: o app do ntfy manda o que a pessoa digitou
static const _Word _WORDS[] PROGMEM = {
    {"ligar", OP_ON}, {"liga", OP_ON}, {"on", OP_ON}, {"abrir", OP_ON}, {"open", OP_ON},
    {"desligar", OP_OFF}, {"desliga", OP_OFF}, {"off", OP_OFF},
    {"fechar", OP_OFF}, {"close", OP_OFF},
    {"alternar", OP_TOGGLE}, {"toggle", OP_TOGGLE},
    {"pulso", OP_PULSE}, {"pulse", OP_PULSE},
    {"nivel", OP_LEVEL}, {"n\xc3\xadvel", OP_LEVEL}, {"level", OP_LEVEL},
    {"brilho", OP_LEVEL}, {"brightness", OP_LEVEL},
    {"potencia", OP_LEVEL}, {"pot\xc3\xaancia", OP_LEVEL},
    {"angulo", OP_ANGLE}, {"\xc3\xa2ngulo", OP_ANGLE}, {"angle", OP_ANGLE},
    {"cor", OP_COLOR}, {"color", OP_COLOR},
    {"auto", OP_AUTO}, {"automatico", OP_AUTO}, {"autom\xc3\xa1tico", OP_AUTO},
    {"status", OP_STATUS}, {"estado", OP_STATUS},
};

// Faixa de cada valor; o mínimo e o máximo valem para o que vier depois
// da palavra. COLOR é lido à parte (hexadecimal).
inline bool _range(uint8_t op, uint32_t& lo, uint32_t& hi) {
    switch (op) {
        case OP_PULSE: lo = 1; hi = 3600; return true;
        case OP_LEVEL: lo = 0; hi = 100;  return true;
        case OP_ANGLE: lo = 0; hi = 180;  return true;
        default:       return false;
    }
}

// Decimal sem sinal, com sufixo opcional (s ou %). Recusa vazio e lixo.
inline bool _number(const char* s, char suffix, uint32_t& out) {
    uint32_t v = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (++digits > 5) return false;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    if (digits == 0) return false;
    if (*s == suffix && suffix) s++;
    if (*s) return false;
    out = v;
    return true;
}

inline bool _hexColor(const char* s, uint32_t& out) {
    if (*s == '#') s++;
    uint32_t v = 0;
    int n = 0;
    for (; *s; s++, n++) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else return false;
        v = (v << 4) | d;
    }
    if (n != 6) return false;
    out = v;
    return true;
}

// Texto → comando. Falso se a palavra ou o valor não valem.
inline bool parseCommand(const char* text, Command& out) {
    char buf[40];
    size_t n = 0;
    while (*text == ' ') text++;
    for (; *text && n < sizeof(buf) - 1; text++) buf[n++] = (char)tolower((unsigned char)*text);
    if (*text) return false;               // longo demais
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';
    if (n == 0) return false;

    char* arg = strchr(buf, ' ');

    // "50%" sozinho é o nível
    if (!arg && buf[n - 1] == '%') {
        uint32_t v = 0;
        if (!_number(buf, '%', v) || v > 100) return false;
        out.op = OP_LEVEL;
        out.value = v;
        return true;
    }

    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') arg++;
    }

    uint8_t op = OP_NONE;
    for (const _Word& w : _WORDS) {
        if (strcmp_P(buf, w.word) == 0) {
            op = pgm_read_byte(&w.op);
            break;
        }
    }
    if (op == OP_NONE) return false;

    uint32_t value = 0;
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (op == OP_COLOR) {
        if (!arg || !_hexColor(arg, value)) return false;
    } else if (_range(op, lo, hi)) {
        char suffix = op == OP_PULSE ? 's' : (op == OP_LEVEL ? '%' : '\0');
        if (!arg || !_number(arg, suffix, value) || value < lo || value > hi) return false;
    } else if (arg) {
        return false;                       // "ligar 5" não existe
    }
    out.op = op;
    out.value = value;
    return true;
}

// O comando faz sentido para esta capacidade?
inline bool commandAllowed(uint8_t cap, uint8_t op) {
    if (cap == C_NONE) return false;
    if (op == OP_LEVEL) return cap == C_LEVEL || cap == C_COLOR;
    if (op == OP_ANGLE) return cap == C_POSITION;
    if (op == OP_COLOR) return cap == C_COLOR;
    return true;
}

} // namespace iot
