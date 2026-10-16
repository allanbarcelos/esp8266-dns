// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// IoT page (the setup form) and the IoT card on Home (the reading and the
// controls). Embedded in the firmware, gzipped (tools/embed_web.py →
// WebAssets.h). The page defines beforehand, in its own <script>, SK and AC
// (type → Kind/Cap, generated from the catalog), T (CSRF), R (the saved
// rule: limit and the value it watches) and D (the unit and what it needs,
// the Pulse seconds, the actuator's name as a text marker), and loads
// i18n.js: L() with the English text gives it in the panel language (the
// build swaps the English for its row in web/i18n.tsv), IX() a marker.
//
// The minifier only joins lines and strips spaces outside quotes: end every
// statement with ; or }, with no regex literal, no template string and no
// division (x * Math.pow(y, -1), in dv()).

function $(i) { return document.getElementById(i); }
function V(n) { return document.querySelector('[name=' + n + ']'); }
function dv(a, b) { return a * Math.pow(b, -1); }
function r1(x) { return +x.toFixed(1); }
function cl(x) { return Math.max(0, Math.min(1, x)); }

// Unit options by sensor kind (the select's values are 0, 1, 2)
var TU = ['°C', '°F'],
    UO = { 2: TU, 3: TU, 4: TU, 7: TU,
        5: [L('Distance (cm)'), L('Tank level (%)'), L('Tank volume (L)')],
        6: [L('Pulses per 10 s'), L('Flow (L/min)'), L('Flow (mL/min)')] },
    US = { 5: ['cm', '%', 'L'], 6: [L('pulses'), 'L/min', 'mL/min'] };

// ── Form: only the fields for the chosen role, types and unit, the wiring
// hint for each type and the rule written as a sentence ────────────────────

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

// The unit select: the options for this kind, the first if the chosen one has none
function uo(k) {
    var s = V('unit'), o = UO[k] || [], i, e;
    for (i = 0; i < 3; i++) {
        e = s.options[i];
        e.text = o[i] || '';
        e.hidden = e.disabled = !o[i];
    }
    if (!o[s.selectedIndex]) s.selectedIndex = 0;
    return +s.value;
}

function f() {
    var r = (document.querySelector('[name=role]:checked') || {}).value || 'n',
        hs = r.indexOf('s') >= 0,
        ha = r.indexOf('a') >= 0,
        k = SK[V('sensor').value],
        p = AC[V('actuator').value],
        n = uo(k),
        d = k == 1,
        h = k == 2 || k == 3 || k == 7,
        u = h && V('metric').value != '1' ? '%' : US[k] ? US[k][n] : TU[n],
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
    $('su').hidden = d;
    $('st').hidden = k != 5 || !n;
    $('sl').hidden = n != 2;
    $('sq').hidden = k != 6 || !n;
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

// ── Home: the sensor drawn by what it measures ─────────────────────────────
// A gauge per value on the scale of the sensor's datasheet (in the unit set,
// with the saved rule's limit marked); a tank for a level or a volume; the
// flow and the volume so far for a flow meter; a lamp with the state's name
// for a digital sensor; the count for pulses.

// Kind → scale: [temperature from, to, humidity from, to]
var GR = { 2: [0, 50, 20, 90], 3: [-40, 80, 0, 100], 4: [-55, 125], 7: [-40, 125, 0, 100] };

// Temperature (°C from the device) in the unit set
function F(c) { return D.u == 1 ? r1(c * 1.8 + 32) : c; }

// Gauge: n label, v value, lo–hi scale, un unit, t limit to mark (NaN: none)
function g(n, v, lo, hi, un, t) {
    var A = " d='M10 50A40 40 0 0 1 90 50'/>",
        Q = function (x) { return cl(dv(x - lo, hi - lo)); };
    return "<div><svg viewBox='0 0 100 62'><path" + A +
        "<path class='gv' pathLength='1' stroke-dasharray='" + Q(v) + " 2'" + A +
        (t == t ? "<line x1='3' y1='50' x2='17' y2='50' transform='rotate(" + 180 * Q(t) + " 50 50)'/>" : '') +
        "<text class='gn' x='50' y='46'>" + v + ' ' + un + "</text><text x='10' y='61'>" + lo +
        "</text><text x='90' y='61'>" + hi + '</text></svg>' + n + '</div>';
}

// Tank: p the level 0–1, s the text inside, t the limit 0–1 (NaN: none)
function tk(p, s, t) {
    var y = r1(92 - 84 * p), z = r1(92 - 84 * t);
    return "<div><svg class='tk' viewBox='0 0 60 100'><rect x='6' y='8' width='48' height='84' rx='6'/><rect class='gv' x='6' y='" + y +
        "' width='48' height='" + r1(92 - y) + "' rx='6'/>" +
        (t == t ? "<line x1='2' y1='" + z + "' x2='58' y2='" + z + "'/>" : '') +
        "<text class='gn' x='30' y='56'>" + s + '</text></svg>' + L('Level') + '</div>';
}

// A number in large print, its unit and a line under it
function big(v, un, sub) {
    return "<div><p><b>" + v + '</b> ' + un + '</p>' + (sub ? "<p class='m'>" + sub + '</p>' : '') + '</div>';
}

// HTML for the reading; '' until there is a value (the row's text says why)
function sv(e) {
    var k = SK[e.type], r = GR[k], t = R[0], u = D.u, p, q, h = '';
    if (e.pulses != null) {
        q = dv(e.pulses * 6, D.p);
        h = !u ? big(e.pulses, L('pulses') + ' ' + L('in 10 s'))
            : u == 1 ? big(r1(q), 'L/min', L('Total') + ': ' + r1(dv(e.total, D.p)) + ' L')
            : big(Math.round(q * 1000), 'mL/min', L('Total') + ': ' + Math.round(dv(e.total * 1000, D.p)) + ' mL');
    }
    if (e.distance != null) {
        p = cl(dv(D.e - e.distance, D.e - D.f));
        h = !u ? g(L('Distance (HC-SR04)'), e.distance, 0, 400, 'cm', t)
            : tk(p, u == 1 ? Math.round(p * 100) + '%' : Math.round(p * D.l) + ' L', cl(dv(t, u == 1 ? 100 : D.l)));
    }
    if (e.temperature != null)
        h = g(L('Temperature'), F(e.temperature), F(r[0]), F(r[1]), TU[u], k == 4 || R[1] ? t : NaN);
    if (e.humidity != null) h += g(L('Humidity'), e.humidity, r[2], r[3], '%', R[1] ? NaN : t);
    return h;
}

function z(e) {
    var G = $('gg'), h = SK[e.type] == 1 ? "<p id='lp'></p>" : sv(e);
    G.innerHTML = h || "<p class='m' id='lp'></p>";
    if ($('lp')) {
        $('lp').textContent = LV(e.text);
        $('lp').className = e.active ? 'on' : h ? '' : 'm';
    }
}

// ── Home: the actuator's controls, by what it does ─────────────────────────
// On/off: a switch. Level (LED, fan, motor): a switch, a 0–100 % slider and
// steps of 10 %. Position (servo): open, close and the angle, on a dial.
// Color (strip): a switch, brightness, a color and a few ready ones. All:
// a pulse of D.s seconds and, with a rule, the way back to automatic.
// Commands go without reloading; a slider being moved is not overwritten.

var SW = ['#ffffff', '#ffb46b', '#ff0000', '#00ff00', '#0000ff'];

function rg(id, n, max, un) {
    return '<label>' + n + ": <output id='" + id + "o'></output>" + un + "<input type='range' id='" + id +
        "' min='0' max='" + max + "'></label>";
}

function ui(c) {
    var b = "<button class='b2' data-c='",
        h = "<div class='ah'><b>" + IX(D.a) + "</b><span id='na'></span></div><div class='ctl'>", i;
    if (c == 3) h += b + "open'>" + L('Open') + '</button>' + b + "close'>" + L('Close') + '</button>';
    else h += "<button role='switch' id='sw' class='tg' aria-checked='false' aria-label='" +
        IX(D.a) + "'><i></i></button>";
    if (c == 2) h += b + "-10' aria-label='-10%'>-</button>" + b + "+10' aria-label='+10%'>+</button>";
    h += b + 'pulse ' + D.s + "'>" + L('Pulse') + ' ' + D.s + ' s</button>';
    if (R[0] == R[0]) h += b + "auto' id='au' hidden>" + L('Back to automatic') + '</button>';
    h += '</div>';
    if (c == 2 || c == 4) h += rg('lv', c == 2 ? L('Level') : L('Brightness'), 100, ' %');
    if (c == 3) h += "<div id='dl'></div>" + rg('ag', L('Angle'), 180, '°');
    if (c == 4) {
        h += "<label>" + L('Color') + "<input type='color' id='co'></label><div class='ctl'>";
        for (i = 0; i < SW.length; i++)
            h += "<button class='sws' data-c='color " + SW[i] + "' style='background:" + SW[i] +
                "' aria-label='" + SW[i] + "'></button>";
        h += '</div>';
    }
    if (R[0] == R[0]) h += "<p class='m' id='nm'></p>";
    return h + "<p id='msg' class='m' role='status' aria-live='polite'></p>";
}

// A step of the level, within 0–100
function st(d) { return 'level ' + Math.max(0, Math.min(100, +$('lv').value + d)); }

function bind(C) {
    C.addEventListener('click', function (v) {
        var b = v.target.closest('[data-c]'), x;
        if (v.target.closest('#sw')) c($('sw').getAttribute('aria-checked') == 'true' ? 'off' : 'on');
        if (!b) return;
        x = b.getAttribute('data-c');
        c(x == '-10' || x == '+10' ? st(+x) : x);
    });
    C.addEventListener('input', function (v) {
        var e = v.target;
        if (e.type == 'range') $(e.id + 'o').value = e.value;
        if (e.id == 'ag') $('dl').innerHTML = g(L('Angle'), e.value, 0, 180, '°', NaN);
    });
    C.addEventListener('change', function (v) {
        var e = v.target;
        if (e.id == 'lv') c('level ' + e.value);
        if (e.id == 'ag') c('angle ' + e.value);
        if (e.id == 'co') c('color ' + e.value);
    });
}

// A value from the device into a control the user is not moving
function put(id, v) {
    var e = $(id);
    if (e && v != null && e != document.activeElement) {
        e.value = v;
        if ($(id + 'o')) $(id + 'o').value = v;
    }
}

function u(s) {
    var a = s.actuator, e = s.sensor, w;
    if (e) z(e);
    if (a) {
        $('na').textContent = LV(a.text);
        if (w = $('sw')) w.setAttribute('aria-checked', a.on);
        if (w = $('nm')) w.textContent = L('Mode') + ': ' + (a.mode != 'auto' ? L('manual') : a.hours === false ? L('automatic, outside the window') : L('automatic'));
        if (w = $('au')) w.hidden = a.mode == 'auto';
        put('lv', a.level);
        put('ag', a.angle);
        put('co', a.color);
        if (a.angle != null) $('dl').innerHTML = g(L('Angle'), a.angle, 0, 180, '°', NaN);
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

// Home draws the card once, from what is set up; the readings come every
// 5 s. A tab out of sight does not ask: each reading is a request (and,
// through the remote access relay, one in its queue).
function home(C) {
    C.innerHTML = (D.k ? "<div id='gg'></div>" : '') + (D.c ? "<div class='ci'>" + ui(D.c) + '</div>' : '');
    bind(C);
    q();
    setInterval(function () { if (!document.hidden) q(); }, 5000);
}

if (V('role')) {
    document.querySelectorAll('form input,form select').forEach(function (e) {
        e.addEventListener('input', f);
    });
    f();
}
if ($('io')) home($('io'));
