// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Panel text, in the browser. The device sends a marker (` plus two base-62
// characters, the row) in place of each text; this file carries the text of
// every language in one matrix (web/i18n.tsv, embedded by tools/embed_web.py)
// and writes the page in the chosen language. It comes gzipped and its URL
// carries a hash of the content: the browser keeps it in the cache and only
// downloads it again when a firmware update changes the text.
//
// The language comes after the # in this script's URL (Settings); without
// one, it is the browser's. The minifier only joins lines and strips spaces
// outside quotes: no regex literal and no template string.

// One column per language (en|pt|es|fr), rows separated by ~. An empty
// cell is the English text and ^ repeats the language before it.
var IM = "@MATRIX@".split('|').map(function (c) { return c.split('~'); }),
    IG = ['en', 'pt', 'es', 'fr'],
    IB = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz',
    IL = (function () {
        var h = (document.currentScript.src.split('#')[1] || ''), l = navigator.languages || [navigator.language || ''], i;
        for (i = -1; i < l.length; i++) {
            var c = (i < 0 ? h : l[i]).slice(0, 2).toLowerCase();
            if (IG.indexOf(c) > 0) return IG.indexOf(c);
            if (c == 'en') return 0;
        }
        return 0;
    })(),
    IT = [], IE = {};
IM[0].forEach(function (en, n) {
    var k = IL;
    while (k > 0 && IM[k][n] == '^') k--;
    IT[n] = IM[k][n] || en;
    IE[en] = n;
});

// Text of a row (number) or of an English text, with {0}, {1}... filled in;
// an unknown text comes back as it is
function L(x) {
    var t = typeof x == 'number' ? IT[x] : IE[x] == null ? x : IT[IE[x]], a = arguments;
    return t.replace(new RegExp('\\{(\\d)\\}', 'g'), function (m, i) { return a[+i + 1]; });
}

// A value from the device (the same English as the API): the whole text or
// its first word ("on 60%", "open 90 deg")
function LV(s) {
    var i = s.indexOf(' ');
    if (IE[s] != null || i < 0 || IE[s.slice(0, i)] == null) return L(s);
    return L(s.slice(0, i)) + s.slice(i);
}

// Markers in a text: the rest of the text node goes into {0}, if the text
// has one ("{0} ago"), or stays after it. A ` that is not a marker (typed
// by someone, in an SSID or a log line) stays as it is.
function IX(s) {
    return s.replace(new RegExp('`([0-9A-Za-z]{2})([^`]*)', 'g'), function (m, id, rest) {
        var t = IT[IB.indexOf(id[0]) * 62 + IB.indexOf(id[1])];
        if (t == null) return m;
        return t.indexOf('{0}') < 0 ? t + rest : t.replace('{0}', rest);
    });
}

// QR code (SVG) of an ASCII text of up to 104 characters: version 5-L
// (37x37, one block of 108 data and 26 correction bytes), byte mode, mask 0.
// One fixed size keeps it short; any reader takes any mask.
function QR(s) {
    var N = 37, m = [], f = [], d = [], b = [], ex = [], lg = [], G = [1], i, j, x, r, c, p = '';
    function set(r, c, v) { m[r * N + c] = v; f[r * N + c] = 1; }
    function put(v, n) { while (n--) b.push(v >> n & 1); }
    function mul(a, y) { return a && y ? ex[(lg[a] + lg[y]) % 255] : 0; }
    function ring(r, c) { return Math.max(Math.abs(r), Math.abs(c)); }
    for (i = 0, x = 1; i < 255; i++, x = x << 1 ^ (x & 128 ? 285 : 0)) { ex[i] = x; lg[x] = i; }
    for (i = 0; i < 26; i++) for (G.push(0), j = i + 1; j > 0; j--) G[j] ^= mul(G[j - 1], ex[i]);
    put(4, 4); put(s.length, 8);
    for (i = 0; i < s.length; i++) put(s.charCodeAt(i), 8);
    put(0, 4);
    while (b.length & 7) b.push(0);
    for (i = 0; i < b.length; i += 8) d.push(parseInt(b.slice(i, i + 8).join(''), 2));
    for (i = 0; d.length < 108; i++) d.push(i & 1 ? 17 : 236);
    x = d.concat(G.slice(1).fill(0));
    for (i = 0; i < 108; i++) for (r = x[i], j = 0; j < 27; j++) x[i + j] ^= mul(G[j], r);
    d = d.concat(x.slice(108));
    // Finders with their white border (corners), timing, alignment, format (L, mask 0)
    [[0, 0], [0, N - 7], [N - 7, 0]].forEach(function (o) {
        for (r = -1; r < 8; r++) for (c = -1; c < 8; c++) {
            x = ring(r - 3, c - 3);
            if (o[0] + r >= 0 && o[0] + r < N && o[1] + c >= 0 && o[1] + c < N) set(o[0] + r, o[1] + c, x != 2 && x != 4 ? 1 : 0);
        }
    });
    for (i = 8; i < N - 8; i++) { set(6, i, i & 1 ^ 1); set(i, 6, i & 1 ^ 1); }
    for (r = -2; r < 3; r++) for (c = -2; c < 3; c++) set(30 + r, 30 + c, ring(r, c) != 1 ? 1 : 0);
    for (i = 0; i < 15; i++) {
        x = 30660 >> i & 1;
        set(i < 6 ? i : i < 8 ? i + 1 : 8, i < 8 ? 8 : i < 9 ? 7 : 14 - i, x);
        set(i < 8 ? 8 : N - 15 + i, i < 8 ? N - 1 - i : 8, x);
    }
    set(N - 8, 8, 1);
    // Data in two-column zigzags from the bottom right, then mask 0
    for (i = 0, c = N - 1; c > 0; c -= 2) {
        if (c == 6) c = 5;
        for (j = 0; j < N * 2; j++) {
            x = c - (j & 1);
            r = (c + 1) & 2 ? j >> 1 : N - 1 - (j >> 1);
            if (!f[r * N + x]) m[r * N + x] = (i < 1072 && d[i >> 3] >> 7 - (i++ & 7) & 1) ^ ((r + x) & 1 ^ 1);
        }
    }
    for (r = 0; r < N; r++) for (c = 0; c < N; c++) if (m[r * N + c]) p += 'M' + c + ' ' + r + 'h1v1h-1z';
    return '<svg class=q viewBox="-4 -4 45 45"><path d="M-4-4h45v45h-45z" fill="#fff"/><path d="' + p + '"/></svg>';
}

(function () {
    var w = document.createTreeWalker(document.body, 4), n, a = ['placeholder', 'title', 'aria-label', 'alt', 'label'];
    while ((n = w.nextNode())) if (n.nodeValue.indexOf('`') >= 0) n.nodeValue = IX(n.nodeValue);
    document.querySelectorAll('[placeholder],[title],[aria-label],[alt],[label]').forEach(function (e) {
        a.forEach(function (k) {
            var v = e.getAttribute(k);
            if (v && v.indexOf('`') >= 0) e.setAttribute(k, IX(v));
        });
    });
    // Drawings (SVG images) keep their text in English: in another language
    // the browser reads the file (already in its cache), translates each
    // text it knows and shows the copy. Hidden meanwhile, so the English
    // does not flash; on any failure the original shows.
    if (IL) document.querySelectorAll('img[src*=".svg"]').forEach(function (img) {
        img.style.visibility = 'hidden';
        fetch(img.src).then(function (r) { return r.text(); }).then(function (s) {
            var d = new DOMParser().parseFromString(s, 'image/svg+xml'), w = d.createTreeWalker(d.documentElement, 4), n, t;
            while ((n = w.nextNode())) {
                t = n.nodeValue.trim();
                if (IE[t] != null) n.nodeValue = n.nodeValue.replace(t, IT[IE[t]]);
            }
            img.src = URL.createObjectURL(new Blob([new XMLSerializer().serializeToString(d)], { type: 'image/svg+xml' }));
        }).catch(function () {}).then(function () { img.style.visibility = ''; });
    });
    // The authenticator key (/mfa) also as a QR code for the app's camera,
    // named after the device
    document.querySelectorAll('.k').forEach(function (k) {
        var l = encodeURIComponent(document.title);
        k.insertAdjacentHTML('afterend', QR('otpauth://totp/' + (l.length > 40 ? 'ESP8266' : l) +
            '?secret=' + k.textContent.split(' ').join('')));
    });
    // The side menu, where the device sends <nav id='mn'>: a link and an
    // icon (24x24 stroke path) per page, in groups; open from the button at
    // the top on narrow screens, always open on wide ones. Then the footer's
    // credit and repository link (the version is already there).
    var mn = document.getElementById('mn'), ft = document.querySelector('footer'), h = '', i, p = location.pathname,
        hd = document.querySelector('header'), de = document.documentElement,
        ic = function (d) { return '<svg viewBox="0 0 24 24" aria-hidden=true><path d="' + d + '"/></svg>'; },
        ln = ['/', 'Home', 'M3 11l9-8 9 8M5 9v12h14V9',
              '/iot', 'IoT', 'M7 7h10v10H7zM10 2v5M14 2v5M10 17v5M14 17v5M2 10h5M2 14h5M17 10h5M17 14h5', 0,
              '/cloudflare', 'Cloudflare', 'M7 19h10a4 4 0 0 0 0-8a6 6 0 0 0-11.6 1.5A3.5 3.5 0 0 0 7 19z',
              '/ntfy', 'Ntfy', 'M6 9a6 6 0 0 1 12 0c0 6 3 8 3 8H3s3-2 3-8M10 21h4',
              '/mqtt', 'MQTT', 'M4 12a8 8 0 0 1 16 0M8 12a4 4 0 0 1 8 0M12 12v9', 0,
              '/config', 'Settings', 'M4 6h16M4 12h16M4 18h16M9 4v4M15 10v4M8 16v4',
              '/update', 'Update', 'M21 12a9 9 0 1 1-2.6-6.4L21 8M21 3v5h-5',
              '/status', 'Status', 'M3 12h4l3-8 4 16 3-8h4',
              '/log', 'Logs', 'M8 6h13M8 12h13M8 18h13M3 6h1M3 12h1M3 18h1'];
    for (i = 0; i < ln.length; i += 3) {
        if (!ln[i]) { h += '<hr>'; i -= 2; continue; }
        h += '<a href=' + ln[i] + (p == ln[i] || ln[i] != '/' && !p.indexOf(ln[i] + '/') ? ' aria-current=page>' : '>') +
            ic(ln[i + 2]) + L(ln[i + 1]) + '</a>';
    }
    if (mn) {
        mn.setAttribute('aria-label', L('Menu'));
        mn.insertAdjacentHTML('afterbegin', h);
        mn.querySelectorAll('form.out button').forEach(function (b) { b.insertAdjacentHTML('afterbegin', ic('M9 21H4V3h5M16 17l5-5-5-5M21 12H9')); });
        hd.insertAdjacentHTML('afterbegin', '<button type=button class=hb aria-controls=mn aria-expanded=false aria-label="' +
            L('Menu') + '">' + ic('M3 6h18M3 12h18M3 18h18') + '</button>');
        de.classList.add('m');
        // The button opens and closes it; a click outside it or Esc closes it
        var mb = hd.firstChild, sh = function (o) { de.classList.toggle('o', o); mb.setAttribute('aria-expanded', o); };
        document.addEventListener('click', function (e) {
            if (mb.contains(e.target)) sh(!de.classList.contains('o'));
            else if (!mn.contains(e.target)) sh(false);
        });
        document.addEventListener('keydown', function (e) { if (e.key == 'Escape') sh(false); });
    }
    if (ft) ft.insertAdjacentHTML('beforeend', '<span>Created by Barcelos.Dev</span>' +
        '<a class=gh href=https://github.com/allanbarcelos/esp8266-dns aria-label=GitHub>' +
        ic('M9 20c-4 1-4-2-6-3m12 4v-4a3 3 0 0 0-1-3c3 0 6-1 6-6a5 5 0 0 0-1-3a5 5 0 0 0 0-3s-1 0-3 1a12 12 0 0 0-6 0C7 2 6 2 6 2a5 5 0 0 0 0 3a5 5 0 0 0-1 3c0 5 3 6 6 6a3 3 0 0 0-1 3v4') + '</a>');
    // Values in the rows (states and statuses from the device)
    document.querySelectorAll('.r b').forEach(function (b) { b.textContent = L(b.textContent); });
    document.documentElement.lang = IG[IL];
    de.classList.add('i');
})();

// A form with an authenticator code sends the browser's clock (seconds) in
// t. The device uses it only while its own clock is not synchronized. A
// form with data-q (a marker) asks that question before it goes.
document.addEventListener('submit', function (e) {
    var t = e.target.querySelector('input[name=t]'), q = e.target.getAttribute('data-q');
    if (q && !confirm(IX(q))) return e.preventDefault();
    if (t) t.value = String(Date.now()).slice(0, -3);
}, true);
