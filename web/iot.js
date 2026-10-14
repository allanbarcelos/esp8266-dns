// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// IoT page. Embedded in the firmware, gzipped (tools/embed_web.py →
// WebAssets.h). The page defines beforehand, in its own <script>, SK and AC
// (type → Kind/Cap, generated from the catalog) and T (CSRF), and loads
// i18n.js: L() with the English text gives it in the panel language (the
// build swaps the English for its row in web/i18n.tsv).
//
// The minifier only joins lines and strips spaces outside quotes: end every
// statement with ; or }, with no regex literal and no template string.

function $(i) { return document.getElementById(i); }
function V(n) { return document.querySelector('[name=' + n + ']'); }

// ── Form: only the fields for the chosen role and types, the wiring hint
// for each type and the rule written as a sentence ─────────────────────────

var SH = ['',
        L('Module with a digital output (D0); sensitivity is set on the trimpot.'),
        L('DHT11: 4.7 to 10 kOhm pull-up between data and 3.3 V; one reading every 2 s.'),
        L('DHT22: 4.7 to 10 kOhm pull-up between data and 3.3 V; one reading every 2 s.'),
        L('DS18B20: 4.7 kOhm pull-up between data and 3.3 V.'),
        L('HC-SR04: trig and echo together on one pin, with 1 kOhm on echo; up to 400 cm.'),
        L('Counts pulses every 10 s (YF-S201: about 75 pulses per L/min).'),
        L('I2C: SDA on this pin and SCL on its pair (GPIO0 with GPIO2, RX with TX); 4.7 kOhm pull-ups on both and address 0x44 (ADDR to GND).')],
    AH = ['',
        L('Relay modules usually switch on low.'),
        L('PWM on the pin: use a MOSFET or a motor driver, never the load straight on the pin.'),
        L('Servo on a separate 5 V supply with GND shared with the ESP.'),
        L('WS2812 on 5 V: 330 Ohm resistor on the data line; up to 60 LEDs.')],
    P = ' ' + L('GPIO0 and GPIO2 must be high at boot; TX and RX stop being serial.');

function f() {
    var r = (document.querySelector('[name=role]:checked') || {}).value || 'n',
        hs = r.indexOf('s') >= 0,
        ha = r.indexOf('a') >= 0,
        k = SK[V('sensor').value],
        p = AC[V('actuator').value],
        d = k == 1,
        h = k == 2 || k == 3 || k == 7,
        u = h ? (V('metric').value == '1' ? '\u00b0C' : '%') : k == 4 ? '\u00b0C' : k == 5 ? 'cm' : L('pulses'),
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
    $('rt').hidden = !ru;
    $('rw').hidden = !V('window').checked;
    $('sh').textContent = SH[k] + P;
    $('ah').textContent = AH[p] + P;
    if (!ru) s = L('No rule: the actuator only obeys commands.');
    else if (d) {
        var o = V('sensor_on').value || L('active');
        s = V('toggle').value == '1' ? L('Each time the sensor becomes {0}, the {1} flips.', o, a)
            : L('Turns the {1} on while the sensor is {0}.', o, a);
    }
    else if (ln) s = L('Level proportional from {0} to {1} {2}.', t, V('threshold_hi').value, u);
    else s = ab ? L('Turns the {0} on when the value goes above {1} {2} and off when it goes back below {3} {2}.', a, t, u, t - m)
        : L('Turns the {0} on when the value goes below {1} {2} and off when it goes back above {3} {2}.', a, t, u, t + m);
    if (ru && V('window').checked)
        s += ' ' + L('Only between {0} and {1} (local time); off outside it.', V('win_start').value, V('win_end').value);
    $('rs').textContent = s;
}
document.querySelectorAll('form input,form select').forEach(function (e) {
    e.addEventListener('input', f);
});
f();

// ── "Now" card (only with a sensor or actuator in use): commands without
// reloading and a reading every 5 s ────────────────────────────────────────

function u(s) {
    var a = s.actuator, e = s.sensor, w;
    if (e) $('ns').textContent = LV(e.text);
    if (a) {
        $('na').textContent = LV(a.text);
        if (w = $('sw')) w.setAttribute('aria-checked', a.on);
        if (w = $('nm')) w.textContent = a.mode != 'auto' ? L('manual') : a.hours === false ? L('automatic, outside the window') : L('automatic');
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
        .then(function (s) { if (s.error) $('msg').textContent = L(s.error); else u(s); })
        .catch(function () { $('msg').textContent = L('No answer from the device'); });
}
if ($('ns') || $('na')) setInterval(function () {
    fetch('/iot/now')
        .then(function (r) { if (r.ok) return r.json(); })
        .then(function (s) { if (s) u(s); })
        .catch(function () {});
}, 5000);
