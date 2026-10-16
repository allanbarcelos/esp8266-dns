// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// IoT page and the IoT card on Home. Embedded in the firmware, gzipped
// (tools/embed_web.py → WebAssets.h). The page defines beforehand, in its own
// <script>, SK and AC (type → Kind/Cap, generated from the catalog), T (CSRF)
// and R (the saved rule: limit and the value it watches), and loads
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
        L('I2C: SDA on this pin, SCL on its pair (GPIO0 with GPIO2, RX with TX); 4.7 kOhm pull-ups; address 0x44.')],
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
// Only the IoT page has the form
if (V('role')) {
    document.querySelectorAll('form input,form select').forEach(function (e) {
        e.addEventListener('input', f);
    });
    f();
}

// ── Sensor drawing on the "Now" card (#gg): a gauge per value on the scale
// of the sensor's datasheet, with the saved rule's limit marked; a lamp
// for a digital sensor and the count for pulses ────────────────────────────

// Kind → scale: [temperature from, to, humidity from, to]; distance in cm
var GR = { 2: [0, 50, 20, 90], 3: [-40, 80, 0, 100], 4: [-55, 125], 5: [0, 400], 7: [-40, 125, 0, 100] };

// Gauge: n label, v value, lo–hi scale, un unit, t threshold to mark (NaN: none)
function g(n, v, lo, hi, un, t) {
    var A = " d='M10 50A40 40 0 0 1 90 50'/>",
        Q = function (x) { return Math.max(0, Math.min(1, (x - lo) * Math.pow(hi - lo, -1))); };
    return "<div><svg viewBox='0 0 100 62'><path" + A + "<path class='gv' pathLength='1' stroke-dasharray='" + Q(v) + " 2'" + A +
        (t == t ? "<line x1='3' y1='50' x2='17' y2='50' transform='rotate(" + 180 * Q(t) + " 50 50)'/>" : '') +
        "<text class='gn' x='50' y='46'>" + v + ' ' + un + "</text><text x='10' y='61'>" + lo +
        "</text><text x='90' y='61'>" + hi + '</text></svg>' + n + '</div>';
}

// Values come only with a good reading; until then the row's text says why
function z(e) {
    var G = $('gg'), k = SK[e.type], r = GR[k], t = R[0], h = '';
    if (k == 1) {
        G.innerHTML = "<p id='lp'></p>";
        $('lp').textContent = LV(e.text);
        $('lp').className = e.active ? 'on' : '';
        return;
    }
    if (e.pulses != null) h = '<p><b>' + e.pulses + '</b> ' + L('pulses') + '</p>';
    if (e.distance != null) h = g(L('Distance (HC-SR04)'), e.distance, r[0], r[1], 'cm', t);
    if (e.temperature != null) h = g(L('Temperature'), e.temperature, r[0], r[1], '°C', k == 4 || R[1] ? t : NaN);
    if (e.humidity != null) h += g(L('Humidity'), e.humidity, r[2], r[3], '%', R[1] ? NaN : t);
    G.innerHTML = h;
}

// ── "Now" card (only with a sensor or actuator in use): commands without
// reloading and a reading every 5 s ────────────────────────────────────────

function u(s) {
    var a = s.actuator, e = s.sensor, w;
    if (e) { $('ns').textContent = LV(e.text); z(e); }
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
function q() {
    fetch('/iot/now')
        .then(function (r) { if (r.ok) return r.json(); })
        .then(function (s) { if (s) u(s); })
        .catch(function () {});
}
// A tab out of sight does not ask: each reading is a request (and, through
// the remote access relay, one in its queue)
if ($('ns') || $('na')) { q(); setInterval(function () { if (!document.hidden) q(); }, 5000); }
