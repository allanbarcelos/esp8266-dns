// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// IoT page (the setup form) and the IoT card on Home (the reading and the
// controls). Embedded in the firmware, gzipped (tools/embed_web.py →
// WebAssets.h). The page defines beforehand, in its own <script>, SK and AC
// (type → Kind/Cap, generated from the catalog), T (CSRF), R (the saved
// rule: limit and the value it watches) and D (the unit and what it needs,
// the Pulse seconds, the actuator's name as a text marker); the IoT page
// also CF (the saved setup), SL and AL (type → name marker). It loads
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
    UO = { 2: TU, 3: TU, 4: TU, 7: TU, 8: TU,
        5: [L('Distance (cm)'), L('Tank level (%)'), L('Tank volume (L)')],
        6: [L('Pulses per 10 s'), L('Flow (L/min)'), L('Flow (mL/min)')] },
    US = { 5: ['cm', '%', 'L'], 6: [L('pulses'), 'L/min', 'mL/min'], 9: ['lx'] };

// ── Form: written here from CF (the saved setup), SL and AL (each type's
// name, a text marker); the device keeps only the checks on save ──────────

// Fields: a label around each control, as the rest of the panel
function lb(n, h) { return '<label>' + n + h + '</label>'; }
function op(v, n, on) { return "<option value='" + v + "'" + (on ? ' selected' : '') + '>' + n + '</option>'; }
function sl(n, name, h) { return lb(n, "<select name='" + name + "'>" + h + '</select>'); }
function pr(n, name, a, b, v) { return sl(n, name, op(0, a, !v) + op(1, b, v)); }
function nm(n, name, v, lo, hi) {
    return lb(n, "<input type='number' name='" + name + "' value='" + v + "' min='" + lo + "' max='" + hi + "'>");
}
function sw(name, n, on) {
    return "<label class='sw'><input type='checkbox' role='switch' id='" + name + "' name='" + name + "' value='1'" +
        (on ? ' checked' : '') + '>' + n + '</label>';
}
function dvi(id, h) { return "<div id='" + id + "'>" + h + '</div>'; }
function fs(id, n, h) { return "<fieldset id='" + id + "'><legend>" + n + '</legend>' + h + '</fieldset>'; }
function hm(m) { return ('0' + Math.floor(dv(m, 60))).slice(-2) + ':' + ('0' + m % 60).slice(-2); }
function tm(n, name, m) { return lb(n, "<input type='time' name='" + name + "' value='" + hm(m) + "'>"); }

// The free pins: GPIO2, GPIO0, TX and RX
function pin(name, v) {
    var h = op(-1, L('Choose'), v < 0), P = [2, 'GPIO2', 0, 'GPIO0', 1, 'TX (GPIO1)', 3, 'RX (GPIO3)'], i;
    for (i = 0; i < 8; i += 2) h += op(P[i], P[i + 1], v == P[i]);
    return sl(L('Pin'), name, h);
}

// A type list in groups: g gives each type's group
function ty(name, M, N, G, g, v) {
    var h = '', i, s;
    for (i = 0; i < G.length; i++) {
        h += "<optgroup label='" + G[i] + "'>";
        for (s in M) if (g(M[s]) == i) h += op(s, IX(N[s]), s == v);
        h += '</optgroup>';
    }
    return sl(L('Type'), name, h);
}

function form() {
    var c = CF, hs = c.s && c.sp >= 0, ha = c.a && c.ap >= 0, R4 = ['n', 's', 'a', 'sa'],
        RN = [L('Nothing (IoT off)'), L('Sensor only: measures and reports'), L('Actuator only: obeys commands'), L('Sensor and actuator')],
        r = (hs ? 1 : 0) + (ha ? 2 : 0), h = '', i, un = '';
    for (i = 0; i < 4; i++)
        h += "<label class='sw'><input type='radio' name='role' value='" + R4[i] + "'" + (i == r ? ' checked' : '') + '>' + RN[i] + '</label>';
    for (i = 0; i < 3; i++) un += op(i, '', c.u == i);
    return fs('fp', L('What does this device do?'), h) +
        fs('fs', L('Sensor'),
            ty('sensor', SK, SL, [L('Temperature and humidity'), L('Presence, contact and alarms'), L('Distance, flow and light')],
                function (k) { return k == 1 ? 1 : k == 5 || k == 6 || k == 9 ? 2 : 0; }, c.s) +
            pin('sensor_pin', c.sp) +
            dvi('sd', pr(L('Active with the pin'), 'sensor_high', L('Low'), L('High'), c.sh) +
                lb(L('Active state name'), "<input name='sensor_on' value='" + c.on + "' maxlength='15' placeholder='" + L('type default') + "'>") +
                lb(L('Inactive state name'), "<input name='sensor_off' value='" + c.of + "' maxlength='15' placeholder='" + L('type default') + "'>")) +
            dvi('sm', pr(L('Value used by the rule'), 'metric', L('Humidity'), L('Temperature'), c.m)) +
            dvi('su', sl(L('Unit'), 'unit', un)) +
            dvi('st', nm(L('Distance to the water, tank empty (cm)'), 'tank_empty', c.te, 2, 400) +
                nm(L('Distance to the water, tank full (cm)'), 'tank_full', c.tf, 0, 399) +
                dvi('sl', nm(L('Tank volume when full (L)'), 'tank_l', c.tl, 1, 30000)) +
                "<p class='m'>" + L('Sensor at the top, facing the water; measure both distances with it in place.') + '</p>') +
            dvi('sq', nm(L('Pulses per litre (YF-S201: about 450)'), 'ppl', c.pp, 1, 30000)) + "<p class='m' id='sh'></p>") +
        fs('fa', L('Actuator'),
            ty('actuator', AC, AL, [L('On and off'), L('Level (PWM)'), L('Position'), L('Color')],
                function (p) { return p - 1; }, c.a) +
            pin('actuator_pin', c.ap) +
            dvi('al', pr(L('On with the pin'), 'actuator_high', L('Low (common relay modules)'), L('High'), c.ah)) +
            dvi('ap', nm(L('Level when on (%)'), 'duty_on', c.d1, 0, 100) + nm(L('Level when off (%)'), 'duty_off', c.d0, 0, 100)) +
            dvi('aa', nm(L('Open angle'), 'angle_on', c.g1, 0, 180) + nm(L('Closed angle'), 'angle_off', c.g0, 0, 180)) +
            dvi('ac', nm(L('Number of LEDs'), 'leds', c.le, 1, 60)) +
            nm(L('Turn off by itself after (min; 0 = never)'), 'max_on', c.mo, 0, 1440) +
            nm(L('Pulse button on Home (s; 0 = the type\'s)'), 'pulse', c.pu, 0, 3600) +
            sw('restore', L('On power-up, go back to the last command'), c.re) + "<p class='m' id='ah'></p>") +
        fs('fr', L('Rule'),
            sw('rule', L('The sensor drives the actuator (automatic mode)'), c.ru) +
            dvi('rd', pr(L('When the sensor becomes active'), 'toggle', L('On while active'), L('Flip the actuator'), c.to)) +
            dvi('rn', pr(L('On when the value is'), 'above', L('Below the limit'), L('Above the limit'), c.ab) +
                nm(L('Limit'), 'threshold', c.th, -999, 9999) + nm(L('Margin to turn off'), 'margin', c.ma, 0, 50) +
                dvi('rl', pr(L('Level'), 'linear', L('On and off at the limit'), L('Proportional up to the high limit'), c.li) +
                    dvi('rh', nm(L('High limit'), 'threshold_hi', c.hi, -999, 9999)))) +
            dvi('rt', sw('window', L('Only within a time window (local time)'), c.wi) +
                dvi('rw', tm(L('From'), 'win_start', c.w1) + tm(L('To'), 'win_end', c.w2) +
                    "<p class='m'>" + L('Outside it, automatic mode keeps the actuator off; a manual command holds until auto. Time zone: Settings.') + '</p>')) +
            "<p class='m' id='rs'></p>") +
        sw('notify', L('Send readings and changes to ntfy'), c.no) + '<button>' + L('Save') + '</button>';
}

// ── Form: only the fields for the chosen role, types and unit, the wiring
// hint for each type and the rule written as a sentence ────────────────────

// Wiring hint by kind; the I2C sensors share one, with their address
var I2 = L('I2C: SDA on this pin, SCL on its pair (GPIO0 with GPIO2, RX with TX); 4.7 kOhm pull-ups; address'),
    DH = L('DHT11/DHT22: 4.7 to 10 kOhm pull-up between data and 3.3 V; one reading every 2 s.'),
    SH = ['',
        L('Module with a digital output (D0); sensitivity is set on the trimpot.'),
        DH, DH,
        L('DS18B20: 4.7 kOhm pull-up between data and 3.3 V.'),
        L('HC-SR04: trig and echo together on one pin, with 1 kOhm on echo; up to 400 cm.'),
        L('Counts pulses every 10 s (YF-S201: about 75 pulses per L/min).'),
        I2 + ' 0x44.', I2 + ' 0x38.', I2 + ' 0x23.'],
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
    var r = document.querySelector('[name=role]:checked').value,   // the form always checks one
        hs = r.indexOf('s') >= 0,
        ha = r.indexOf('a') >= 0,
        k = SK[V('sensor').value],
        p = AC[V('actuator').value],
        n = uo(k),
        d = k == 1,
        h = k == 2 || k == 3 || k == 7 || k == 8,
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
    $('su').hidden = !UO[k];
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
var GR = { 2: [0, 50, 20, 90], 3: [-40, 80, 0, 100], 4: [-55, 125], 7: [-40, 125, 0, 100], 8: [-40, 85, 0, 100] };

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
    if (e.lux != null) h = big(e.lux, 'lx');
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
// On/off: a switch (a gate only pulses: no switch to hold it on). Level (LED, fan, motor): a switch, a 0–100 % slider and
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
    else if (D.t != 'gate') h += "<button role='switch' id='sw' class='tg' aria-checked='false' aria-label='" +
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

if ($('fm')) {
    $('fm').insertAdjacentHTML('beforeend', form());
    document.querySelectorAll('form input,form select').forEach(function (e) {
        e.addEventListener('input', f);
    });
    f();
}
if ($('io')) home($('io'));
