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
    // Values in the rows (states and statuses from the device)
    document.querySelectorAll('.r b').forEach(function (b) {
        b.textContent = b.id == 'ns' || b.id == 'na' ? LV(b.textContent) : L(b.textContent);
    });
    document.documentElement.lang = IG[IL];
    document.documentElement.className = 'i';
})();
