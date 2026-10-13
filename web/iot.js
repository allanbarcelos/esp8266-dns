// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Página IoT. Vai embutido no firmware em gzip (tools/embed_web.py →
// WebAssets.h). A página define antes, num <script> próprio, SK e AC
// (tipo → Kind/Cap, gerados do catálogo) e T (CSRF).
//
// O minificador só junta linhas e tira espaços fora das aspas: termine todo
// comando com ; ou }, sem regex literal nem template string.

function $(i) { return document.getElementById(i); }
function V(n) { return document.querySelector('[name=' + n + ']'); }

// ── Formulário: só os campos do papel e dos tipos escolhidos, a dica de
// ligação de cada tipo e a regra escrita como frase ─────────────────────────

var SH = ['',
        'Modulo com saida digital (D0); a sensibilidade se ajusta no trimpot.',
        'DHT11: pull-up de 4,7 a 10 kOhm entre dados e 3,3 V; uma leitura a cada 2 s.',
        'DHT22: pull-up de 4,7 a 10 kOhm entre dados e 3,3 V; uma leitura a cada 2 s.',
        'DS18B20: pull-up de 4,7 kOhm entre dados e 3,3 V.',
        'HC-SR04: trig e echo juntos no mesmo pino, com 1 kOhm no echo; ate 400 cm.',
        'Conta pulsos a cada 10 s (YF-S201: cerca de 75 pulsos por L/min).'],
    AH = ['',
        'Modulos de rele costumam ligar em nivel baixo.',
        'PWM no pino: use MOSFET ou driver de motor, nunca a carga direto no pino.',
        'Servo com 5 V separado e GND em comum com o ESP.',
        'WS2812 em 5 V: resistor de 330 Ohm na linha de dados; ate 60 LEDs.'],
    P = ' GPIO0 e GPIO2 precisam estar altos no boot; TX e RX deixam de ser serial.';

function f() {
    var r = (document.querySelector('[name=role]:checked') || {}).value || 'n',
        hs = r.indexOf('s') >= 0,
        ha = r.indexOf('a') >= 0,
        k = SK[V('sensor').value],
        p = AC[V('actuator').value],
        d = k == 1,
        h = k == 2 || k == 3,
        u = h ? (V('metric').value == '1' ? '\u00b0C' : '%') : k == 4 ? '\u00b0C' : k == 5 ? 'cm' : 'pulsos',
        a = V('actuator').selectedOptions[0].text.toLowerCase(),
        t = +V('threshold').value,
        m = +V('margin').value,
        ab = V('above').value == '1',
        ru = V('rule').checked,
        ln = p == 2 && !d && V('linear').value == '1',
        s;
    $('fs').hidden = !hs;
    $('fa').hidden = !ha;
    $('fr').hidden = !(hs && ha);
    $('sd').hidden = !d;
    $('sm').hidden = !h;
    $('al').hidden = p != 1;
    $('ap').hidden = p != 2;
    $('aa').hidden = p != 3;
    $('ac').hidden = p != 4;
    $('rd').hidden = !d || !ru;
    $('rn').hidden = d || !ru;
    $('rl').hidden = !(p == 2 && !d);
    $('rh').hidden = !ln;
    $('sh').textContent = SH[k] + P;
    $('ah').textContent = AH[p] + P;
    if (!ru) s = 'Sem regra: o atuador so obedece comandos.';
    else if (d) {
        var o = V('sensor_on').value || 'ativo';
        s = V('toggle').value == '1' ? 'Cada vez que o sensor ficar ' + o + ', o ' + a + ' inverte.'
            : 'Liga o ' + a + ' enquanto o sensor estiver ' + o + '.';
    }
    else if (ln) s = 'Intensidade proporcional de ' + t + ' a ' + V('threshold_hi').value + ' ' + u + '.';
    else s = 'Liga o ' + a + ' quando o valor ficar ' + (ab ? 'acima de ' : 'abaixo de ') + t + ' ' + u
        + ' e desliga ao voltar ' + (ab ? 'abaixo de ' : 'acima de ') + (ab ? t - m : t + m) + ' ' + u + '.';
    $('rs').textContent = s;
}
document.querySelectorAll('form input,form select').forEach(function (e) {
    e.addEventListener('input', f);
});
f();

// ── Cartão "Agora" (só com sensor ou atuador ligado): comandos sem
// recarregar e leitura a cada 5 s ──────────────────────────────────────────

function u(s) {
    var a = s.actuator, e = s.sensor, w;
    if (e) $('ns').textContent = e.text;
    if (a) {
        $('na').textContent = a.text;
        if (w = $('sw')) w.setAttribute('aria-checked', a.on);
        if (w = $('nm')) w.textContent = a.mode == 'auto' ? 'automatico' : 'manual';
        if (w = $('au')) w.hidden = a.mode == 'auto';
        if (a.level != null && (w = $('lv'))) { w.value = a.level; $('lvo').value = a.level; }
        if (a.angle != null && (w = $('ag'))) { w.value = a.angle; $('ago').value = a.angle; }
        if (a.color && (w = $('co'))) w.value = a.color;
    }
}
function c(x) {
    $('msg').textContent = '';
    fetch('/iot/cmd', { method: 'POST', body: new URLSearchParams({ cmd: x, _csrf: T }) })
        .then(function (r) { return r.json(); })
        .then(function (s) { if (s.error) $('msg').textContent = s.error; else u(s); })
        .catch(function () { $('msg').textContent = 'Sem resposta do aparelho'; });
}
if ($('ns') || $('na')) setInterval(function () {
    fetch('/iot/now')
        .then(function (r) { if (r.ok) return r.json(); })
        .then(function (s) { if (s) u(s); })
        .catch(function () {});
}, 5000);
