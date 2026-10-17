// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Tests of web/iot.js on Node (node --test): the IoT card on Home, drawn
// for each kind of sensor and each capability of actuator, its commands
// and readings, and the setup form on the IoT page. The browser is imitated
// here with the little iot.js uses: elements found by id or name, the HTML
// it writes (each tag with an id becomes an element), events, fetch and a
// timer. i18n.js is imitated too: L() gives the English with {0}… filled.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

// IOT_JS: the same tests on the minified script (tools/embed_web.py --min-iot)
const FILE = process.env.IOT_JS || fileURLToPath(new URL('./iot.js', import.meta.url));
const SRC = readFileSync(FILE, 'utf8');

class El {
  constructor(dom, tag = 'div', attrs = {}) {
    this.dom = dom;
    this.tagName = tag;
    this.attrs = { ...attrs };
    this.id = attrs.id || '';
    this.type = attrs.type || '';
    this.hidden = 'hidden' in attrs;
    this.value = attrs.value ?? '';
    this.className = attrs.class || '';
    this.textContent = '';
    this.checked = 'checked' in attrs;
    this.listeners = {};
    this._html = '';
  }
  getAttribute(n) { return n in this.attrs ? this.attrs[n] : null; }
  setAttribute(n, v) { this.attrs[n] = String(v); }
  closest(sel) {
    if (sel === '[data-c]') return 'data-c' in this.attrs ? this : null;
    return sel === '#' + this.id ? this : null;
  }
  addEventListener(t, f) { (this.listeners[t] ||= []).push(f); }
  fire(t, target = this) { for (const f of this.listeners[t] || []) f({ target }); }
  set innerHTML(h) {
    this._html = h;
    const tag = /<(\w+)((?:\s+[\w-]+(?:='[^']*')?)*)\s*\/?>/g;
    for (const m of h.matchAll(tag)) {
      const attrs = {};
      for (const a of m[2].matchAll(/([\w-]+)(?:='([^']*)')?/g)) attrs[a[1]] = a[2] ?? '';
      if (attrs.id) this.dom.ids[attrs.id] = new El(this.dom, m[1], attrs);
    }
  }
  get innerHTML() { return this._html; }
}

// A <select>: options, selectedIndex and value kept together
class Select extends El {
  constructor(dom, name, values, texts = values, chosen = 0) {
    super(dom, 'select', { name });
    this.options = values.map((v, i) => ({ value: v, text: texts[i], hidden: false, disabled: false }));
    this.selectedIndex = chosen;
  }
  get selectedIndex() { return this._i; }
  set selectedIndex(i) { this._i = i; }
  get value() { return this.options[this._i].value; }
  set value(v) { if (this.options) this._i = this.options.findIndex((o) => o.value === String(v)); }
  get selectedOptions() { return [this.options[this._i]]; }
}

// A page: the globals iot.js reads, the elements, fetch and the timer
function page({ D = {}, R = [NaN, 0], home = false, form = null, reply } = {}) {
  const dom = { ids: {}, names: {}, inputs: [] };
  const calls = [];
  const timers = [];
  const doc = {
    hidden: false,
    activeElement: null,
    getElementById: (i) => dom.ids[i] || null,
    querySelector(sel) {
      if (sel === '[name=role]:checked') return dom.inputs.find((e) => e.attrs.name === 'role' && e.checked) || null;
      const m = sel.match(/^\[name=(\w+)\]$/);
      return (m && dom.names[m[1]]) || null;
    },
    querySelectorAll: () => dom.inputs,
  };
  const ctx = {
    document: doc,
    Math, URLSearchParams, Promise, NaN,
    SK: { dht22: 3, dht11: 2, ds18b20: 4, sht3x: 7, aht20: 8, pir: 1, contact: 1, leak: 1, distance: 5, pulses: 6,
      bh1750: 9 },
    AC: { relay: 1, pump: 1, gate: 1, fan: 2, motor: 2, servo: 3, strip: 4 },
    T: 'csrf-token',
    R,
    D: { k: 0, c: 0, u: 0, e: 100, f: 20, l: 1000, p: 450, s: 1, t: 'relay', a: 'Relay', ...D },
    L: (x, ...a) => String(x).replace(/\{(\d)\}/g, (_, i) => a[i]),
    LV: (s) => s,
    IX: (s) => s,
    setInterval: (f, ms) => timers.push([f, ms]),
    fetch: (url, init) => {
      calls.push({ url, body: init && init.body ? Object.fromEntries(init.body) : null });
      return (reply || (() => ({})))(url, init);
    },
  };
  if (home) dom.ids.io = new El(dom, 'div', { id: 'io' });
  if (form) form(dom);
  vm.createContext(ctx);
  vm.runInContext(SRC, ctx, { filename: FILE });
  return { dom, doc, ctx, calls, timers, $: (i) => dom.ids[i] };
}

// fetch answers: a JSON body, a failed status, or no answer
const json = (o) => () => Promise.resolve({ ok: true, json: () => Promise.resolve(o) });
const settle = () => new Promise((r) => setImmediate(r));

// Home with a state from /iot/now
async function homeWith(D, state, R) {
  const p = page({ D, R, home: true, reply: json(state) });
  await settle();
  return p;
}

const sensorState = (s) => ({ api: 1, sensor: { status: 'ok', ...s }, actuator: null });

// ── Home: the sensor, by what it measures ────────────────────────────────────

test('DHT22: a gauge for each value on the datasheet scale, in °C', async () => {
  const p = await homeWith({ k: 3 }, sensorState({ type: 'dht22', text: '26.5 C, 48.7%', temperature: 26.5, humidity: 48.7 }));
  const h = p.$('gg').innerHTML;
  assert.match(h, /<text class='gn' x='50' y='46'>26\.5 °C<\/text><text x='10' y='61'>-40<\/text><text x='90' y='61'>80<\/text>/);
  assert.match(h, /<\/svg>Temperature<\/div>/);
  assert.match(h, />48\.7 %<\/text><text x='10' y='61'>0<\/text><text x='90' y='61'>100</);
  assert.match(h, /<\/svg>Humidity<\/div>/);
  assert.doesNotMatch(h, /<line/);                                   // no rule: no limit
  // The arc: the share of the scale, (26.5 + 40) / 120
  assert.match(h, /stroke-dasharray='0\.5541666666666667 2'/);
  assert.equal(p.calls[0].url, '/iot/now');
});

test('°F: the value and the scale converted', async () => {
  const p = await homeWith({ k: 3, u: 1 }, sensorState({ type: 'dht22', text: '79.7 F, 48.7%', temperature: 26.5, humidity: 48.7 }));
  assert.match(p.$('gg').innerHTML, />79\.7 °F<\/text><text x='10' y='61'>-40<\/text><text x='90' y='61'>176</);
});

test('the rule limit marks the value it watches', async () => {
  // DS18B20: only a temperature, the limit always on it
  let p = await homeWith({ k: 4 }, sensorState({ type: 'ds18b20', text: '25.0 C', temperature: 25 }), [28, 0]);
  let h = p.$('gg').innerHTML;
  assert.match(h, /<line x1='3' y1='50' x2='17' y2='50' transform='rotate\(83 50 50\)'\/>/);
  assert.match(h, /<text x='10' y='61'>-55<\/text><text x='90' y='61'>125</);
  // DHT11 watching the humidity: the mark on the humidity gauge only
  p = await homeWith({ k: 2 }, sensorState({ type: 'dht11', text: '', temperature: 20, humidity: 55 }), [60, 0]);
  h = p.$('gg').innerHTML;
  const [temp, hum] = h.split('Temperature</div>');
  assert.doesNotMatch(temp, /<line/);
  assert.match(hum, /<line/);
  assert.match(temp, /<text x='10' y='61'>0<\/text><text x='90' y='61'>50</);
  assert.match(hum, /<text x='10' y='61'>20<\/text><text x='90' y='61'>90</);
  // SHT3x watching the temperature: the mark on it, in °F when set
  p = await homeWith({ k: 7, u: 1 }, sensorState({ type: 'sht3x', text: '', temperature: 30, humidity: 40 }), [86, 1]);
  [h] = p.$('gg').innerHTML.split('Temperature</div>');
  assert.match(h, /<line/);
  assert.match(h, />86 °F<\/text><text x='10' y='61'>-40<\/text><text x='90' y='61'>257</);
});

test('a digital sensor: a lamp with the state name, lit when active', async () => {
  let p = await homeWith({ k: 1 }, sensorState({ type: 'pir', text: 'motion', active: true }));
  assert.equal(p.$('gg').innerHTML, "<p id='lp'></p>");
  assert.equal(p.$('lp').textContent, 'motion');
  assert.equal(p.$('lp').className, 'on');
  p = await homeWith({ k: 1 }, sensorState({ type: 'contact', text: 'closed', active: false }));
  assert.equal(p.$('lp').className, '');
});

test('no value yet (or an error): the text says why', async () => {
  for (const s of [{ type: 'dht22', status: 'waiting', text: 'waiting' }, { type: 'distance', status: 'error', text: 'error' }]) {
    const p = await homeWith({ k: 3 }, { sensor: s });
    assert.equal(p.$('gg').innerHTML, "<p class='m' id='lp'></p>");
    assert.equal(p.$('lp').textContent, s.text);
    assert.equal(p.$('lp').className, 'm');
  }
});

test('distance in cm: a gauge from 0 to 400 cm, with the limit', async () => {
  const p = await homeWith({ k: 5 }, sensorState({ type: 'distance', text: '120 cm', distance: 120 }), [50, 0]);
  const h = p.$('gg').innerHTML;
  assert.match(h, />120 cm<\/text><text x='10' y='61'>0<\/text><text x='90' y='61'>400</);
  assert.match(h, /Distance \(HC-SR04\)<\/div>/);
  assert.match(h, /rotate\(22\.5 50 50\)/);
});

test('a tank: the level in % or the volume in L, filled to it', async () => {
  const D = { k: 5, u: 1, e: 110, f: 10, l: 2000 };
  let p = await homeWith(D, sensorState({ type: 'distance', text: '50%', distance: 60 }), [20, 0]);
  let h = p.$('gg').innerHTML;
  assert.match(h, /<svg class='tk' viewBox='0 0 60 100'>/);
  assert.match(h, /<rect class='gv' x='6' y='50' width='48' height='42' rx='6'\/>/);   // half full
  assert.match(h, /<line x1='2' y1='75\.2' x2='58' y2='75\.2'\/>/);                     // 20 %
  assert.match(h, /<text class='gn' x='30' y='56'>50%<\/text><\/svg>Level<\/div>/);
  p = await homeWith({ ...D, u: 2 }, sensorState({ type: 'distance', text: '1000 L', distance: 60 }), [500, 0]);
  h = p.$('gg').innerHTML;
  assert.match(h, />1000 L<\/text>/);
  assert.match(h, /y1='71' x2='58' y2='71'/);                                           // 500 of 2000 L
  // Beyond the marks: empty or full, never past them; no rule: no line
  p = await homeWith(D, sensorState({ type: 'distance', text: '', distance: 300 }));
  assert.match(p.$('gg').innerHTML, /y='92' width='48' height='0'.*>0%</);
  assert.doesNotMatch(p.$('gg').innerHTML, /<line/);
  p = await homeWith(D, sensorState({ type: 'distance', text: '', distance: 2 }));
  assert.match(p.$('gg').innerHTML, /y='8' width='48' height='84'.*>100%</);
});

test('pulses: the count; a flow meter: the flow and the volume so far', async () => {
  let p = await homeWith({ k: 6 }, sensorState({ type: 'pulses', text: '12 pulses/10 s', pulses: 12, total: 40 }));
  assert.equal(p.$('gg').innerHTML, '<div><p><b>12</b> pulses in 10 s</p></div>');
  p = await homeWith({ k: 6, u: 1, p: 450 }, sensorState({ type: 'pulses', text: '', pulses: 75, total: 75 }));
  assert.equal(p.$('gg').innerHTML, "<div><p><b>1</b> L/min</p><p class='m'>Total: 0.2 L</p></div>");
  p = await homeWith({ k: 6, u: 1, p: 450 }, sensorState({ type: 'pulses', text: '', pulses: 100, total: 4500 }));
  assert.equal(p.$('gg').innerHTML, "<div><p><b>1.3</b> L/min</p><p class='m'>Total: 10 L</p></div>");
  p = await homeWith({ k: 6, u: 2, p: 450 }, sensorState({ type: 'pulses', text: '', pulses: 75, total: 75 }));
  assert.equal(p.$('gg').innerHTML, "<div><p><b>1000</b> mL/min</p><p class='m'>Total: 167 mL</p></div>");
});

test('AHT20: two gauges on its scale; BH1750: the light in lux', async () => {
  let p = await homeWith({ k: 8 }, sensorState({ type: 'aht20', text: '', temperature: 22, humidity: 40 }));
  assert.match(p.$('gg').innerHTML, />22 °C<\/text><text x='10' y='61'>-40<\/text><text x='90' y='61'>85</);
  assert.match(p.$('gg').innerHTML, />40 %<\/text><text x='10' y='61'>0<\/text><text x='90' y='61'>100</);
  p = await homeWith({ k: 9 }, sensorState({ type: 'bh1750', text: '250 lx', lux: 250 }));
  assert.equal(p.$('gg').innerHTML, '<div><p><b>250</b> lx</p></div>');
});

// ── Home: the actuator, by what it does ──────────────────────────────────────

const actState = (a) => ({ api: 1, sensor: null, actuator: { mode: 'manual', on: false, text: 'off', ...a } });

test('on/off: a switch named after the actuator and the type\'s pulse', async () => {
  const p = await homeWith({ c: 1, s: 10, a: 'Pump' }, actState({ type: 'pump', capability: 'onoff', on: true, text: 'on' }));
  const h = p.$('io').innerHTML;
  assert.match(h, /^<div class='ci'><div class='ah'><b>Pump<\/b><span id='na'><\/span><\/div>/);
  assert.match(h, /<button role='switch' id='sw' class='tg' aria-checked='false' aria-label='Pump'><i><\/i><\/button>/);
  assert.match(h, /<button class='b2' data-c='pulse 10'>Pulse 10 s<\/button>/);
  assert.doesNotMatch(h, /id='gg'|id='au'|id='nm'|type='range'|data-c='open'/);
  // The reading turns the switch on
  assert.equal(p.$('sw').getAttribute('aria-checked'), 'true');
  assert.equal(p.$('na').textContent, 'on');
  // A tap sends the opposite; the answer is the new state
  p.ctx.fetch = (url, init) => {
    p.calls.push({ url, body: Object.fromEntries(init.body) });
    return json(actState({ on: false, text: 'off' }))();
  };
  p.$('io').fire('click', p.$('sw'));
  await settle();
  assert.deepEqual(p.calls.at(-1), { url: '/iot/cmd', body: { cmd: 'off', _csrf: 'csrf-token' } });
  assert.equal(p.$('sw').getAttribute('aria-checked'), 'false');
  p.$('io').fire('click', p.$('sw'));
  assert.equal(p.calls.at(-1).body.cmd, 'on');
  // The pulse button
  p.$('io').fire('click', new El(p.dom, 'button', { 'data-c': 'pulse 10' }));
  assert.equal(p.calls.at(-1).body.cmd, 'pulse 10');
  // A click elsewhere sends nothing
  const n = p.calls.length;
  p.$('io').fire('click', new El(p.dom, 'span'));
  assert.equal(p.calls.length, n);
});

test('a gate only pulses: no switch to hold it open', async () => {
  const p = await homeWith({ c: 1, s: 1, t: 'gate', a: 'Gate or garage door' }, actState({ type: 'gate' }));
  const h = p.$('io').innerHTML;
  assert.doesNotMatch(h, /id='sw'/);
  assert.match(h, /<b>Gate or garage door<\/b>/);
  assert.match(h, /data-c='pulse 1'>Pulse 1 s</);
});

test('level: a switch, steps of 10 % and a slider that is not overwritten while moved', async () => {
  const p = await homeWith({ c: 2, s: 30, a: 'Fan' }, actState({ type: 'fan', capability: 'level', on: true, text: 'on 60%', level: 60 }));
  const h = p.$('io').innerHTML;
  assert.match(h, /id='sw'/);
  assert.match(h, /data-c='-10' aria-label='-10%'>-<\/button><button class='b2' data-c='\+10' aria-label='\+10%'>\+<\/button>/);
  assert.match(h, /<label>Level: <output id='lvo'><\/output> %<input type='range' id='lv' min='0' max='100'><\/label>/);
  assert.equal(p.$('lv').value, 60);
  assert.equal(p.$('lvo').value, 60);
  p.ctx.fetch = (url, init) => { p.calls.push(Object.fromEntries(init.body)); return new Promise(() => {}); };
  const btn = (d) => new El(p.dom, 'button', { 'data-c': d });
  p.$('io').fire('click', btn('+10'));
  assert.equal(p.calls.at(-1).cmd, 'level 70');
  p.$('io').fire('click', btn('-10'));
  assert.equal(p.calls.at(-1).cmd, 'level 50');
  p.$('lv').value = 95;
  p.$('io').fire('click', btn('+10'));
  assert.equal(p.calls.at(-1).cmd, 'level 100');
  p.$('lv').value = 4;
  p.$('io').fire('click', btn('-10'));
  assert.equal(p.calls.at(-1).cmd, 'level 0');
  // Moving the slider shows the value; letting go sends it
  p.$('lv').value = 35;
  p.$('io').fire('input', p.$('lv'));
  assert.equal(p.$('lvo').value, 35);
  p.$('io').fire('change', p.$('lv'));
  assert.equal(p.calls.at(-1).cmd, 'level 35');
  // A reading while the finger is on the slider leaves it alone
  p.doc.activeElement = p.$('lv');
  p.ctx.fetch = json(actState({ level: 80, on: true }));
  p.timers[0][0]();
  await settle();
  assert.equal(p.$('lv').value, 35);
  p.doc.activeElement = null;
  p.timers[0][0]();
  await settle();
  assert.equal(p.$('lv').value, 80);
});

test('position: open, close and the angle on a dial', async () => {
  const p = await homeWith({ c: 3, s: 3, a: 'Servo' }, actState({ type: 'servo', capability: 'position', angle: 90, ok: true, text: 'open 90 deg' }));
  const h = p.$('io').innerHTML;
  assert.match(h, /<button class='b2' data-c='open'>Open<\/button><button class='b2' data-c='close'>Close<\/button>/);
  assert.doesNotMatch(h, /id='sw'|data-c='-10'/);
  assert.match(h, /<div id='dl'><\/div><label>Angle: <output id='ago'><\/output>°<input type='range' id='ag' min='0' max='180'>/);
  assert.match(p.$('dl').innerHTML, />90 °<\/text><text x='10' y='61'>0<\/text><text x='90' y='61'>180</);
  assert.equal(p.$('ag').value, 90);
  p.ctx.fetch = (url, init) => { p.calls.push(Object.fromEntries(init.body)); return new Promise(() => {}); };
  p.$('io').fire('click', new El(p.dom, 'button', { 'data-c': 'close' }));
  assert.equal(p.calls.at(-1).cmd, 'close');
  p.$('ag').value = 120;
  p.$('io').fire('input', p.$('ag'));
  assert.match(p.$('dl').innerHTML, />120 °</);
  assert.equal(p.$('ago').value, 120);
  p.$('io').fire('change', p.$('ag'));
  assert.equal(p.calls.at(-1).cmd, 'angle 120');
});

test('color: a switch, brightness, a color and ready ones', async () => {
  const p = await homeWith({ c: 4, s: 5, a: 'LED strip' }, actState({ type: 'strip', capability: 'color', on: true, level: 40, color: '#ff8800' }));
  const h = p.$('io').innerHTML;
  assert.match(h, /id='sw'/);
  assert.match(h, /<label>Brightness: <output id='lvo'><\/output> %<input type='range' id='lv'/);
  assert.match(h, /<label>Color<input type='color' id='co'><\/label>/);
  assert.equal(h.match(/class='sws'/g).length, 5);
  assert.match(h, /<button class='sws' data-c='color #ff0000' style='background:#ff0000' aria-label='#ff0000'><\/button>/);
  assert.equal(p.$('co').value, '#ff8800');
  assert.equal(p.$('lv').value, 40);
  p.ctx.fetch = (url, init) => { p.calls.push(Object.fromEntries(init.body)); return new Promise(() => {}); };
  p.$('io').fire('click', new El(p.dom, 'button', { 'data-c': 'color #0000ff' }));
  assert.equal(p.calls.at(-1).cmd, 'color #0000ff');
  p.$('co').value = '#123456';
  p.$('io').fire('change', p.$('co'));
  assert.equal(p.calls.at(-1).cmd, 'color #123456');
  p.$('io').fire('change', new El(p.dom, 'input', { id: 'x', type: 'text' }));
  assert.equal(p.calls.at(-1).cmd, 'color #123456');
});

test('with a rule: the mode and the way back to automatic', async () => {
  const p = await homeWith({ k: 1, c: 1 }, { sensor: { type: 'pir', status: 'ok', text: 'clear', active: false },
    actuator: { type: 'relay', mode: 'auto', on: false, text: 'off' } }, [0, 0]);
  assert.match(p.$('io').innerHTML, /^<div id='gg'><\/div><div class='ci'>/);
  assert.match(p.$('io').innerHTML, /<button class='b2' data-c='auto' id='au' hidden>Back to automatic<\/button>/);
  assert.equal(p.$('nm').textContent, 'Mode: automatic');
  assert.equal(p.$('au').hidden, true);
  const now = (a) => { p.ctx.fetch = json({ actuator: { type: 'relay', on: true, text: 'on', ...a } }); p.timers[0][0](); return settle(); };
  await now({ mode: 'manual' });
  assert.equal(p.$('nm').textContent, 'Mode: manual');
  assert.equal(p.$('au').hidden, false);
  await now({ mode: 'auto', hours: false });
  assert.equal(p.$('nm').textContent, 'Mode: automatic, outside the window');
  await now({ mode: 'auto', hours: true });
  assert.equal(p.$('nm').textContent, 'Mode: automatic');
});

test('a refused command and a device that does not answer are said', async () => {
  const p = await homeWith({ c: 1 }, actState({}));
  p.ctx.fetch = () => Promise.resolve({ json: () => Promise.resolve({ error: 'invalid command' }) });
  p.$('io').fire('click', new El(p.dom, 'button', { 'data-c': 'pulse 1' }));
  await settle();
  assert.equal(p.$('msg').textContent, 'invalid command');
  p.ctx.fetch = () => Promise.reject(new Error('down'));
  p.$('io').fire('click', new El(p.dom, 'button', { 'data-c': 'pulse 1' }));
  assert.equal(p.$('msg').textContent, '');                           // cleared on sending
  await settle();
  assert.equal(p.$('msg').textContent, 'No answer from the device');
});

test('readings every 5 s, not from a hidden tab; a failed one changes nothing', async () => {
  const p = await homeWith({ k: 4 }, sensorState({ type: 'ds18b20', text: '', temperature: 20 }));
  assert.equal(p.timers.length, 1);
  assert.equal(p.timers[0][1], 5000);
  const n = p.calls.length;
  p.doc.hidden = true;
  p.timers[0][0]();
  assert.equal(p.calls.length, n);
  p.doc.hidden = false;
  p.ctx.fetch = (url) => { p.calls.push(url); return Promise.resolve({ ok: false }); };
  p.timers[0][0]();
  await settle();
  assert.match(p.$('gg').innerHTML, />20 °C</);
  p.ctx.fetch = () => Promise.reject(new Error('down'));
  p.timers[0][0]();
  await settle();
  assert.match(p.$('gg').innerHTML, />20 °C</);
});

test('sensor only: no controls; actuator only: no reading area', async () => {
  let p = await homeWith({ k: 4 }, sensorState({ type: 'ds18b20', text: '', temperature: 20 }));
  assert.equal(p.$('io').innerHTML, "<div id='gg'></div>");
  p = await homeWith({ c: 1 }, actState({}));
  assert.doesNotMatch(p.$('io').innerHTML, /id='gg'/);
});

test('a page without the card or the form runs nothing', () => {
  const p = page();
  assert.equal(p.calls.length, 0);
  assert.equal(p.timers.length, 0);
});

// ── IoT page: the setup form ─────────────────────────────────────────────────

const IDS = ['fs', 'fa', 'fr', 'sd', 'sm', 'su', 'st', 'sl', 'sq', 'al', 'ap', 'aa', 'ac', 'rd', 'rn', 'rl', 'rh',
  'rt', 'rw', 'sh', 'ah', 'rs'];

function form(values = {}) {
  const v = { role: 'sa', sensor: 'ds18b20', actuator: 'relay', unit: '0', metric: '1', threshold: '28', margin: '2',
    above: '1', rule: true, linear: '0', threshold_hi: '35', window: false, win_start: '22:00', win_end: '06:00',
    sensor_on: '', toggle: '0', ...values };
  return (dom) => {
    for (const id of IDS) dom.ids[id] = new El(dom, 'div', { id });
    const add = (e) => { dom.names[e.attrs.name] = e; dom.inputs.push(e); return e; };
    for (const r of ['n', 's', 'a', 'sa']) {
      const e = new El(dom, 'input', { type: 'radio', name: 'role', value: r });
      e.checked = r === v.role;
      dom.names.role ||= e;
      dom.inputs.push(e);
    }
    const sensors = ['ds18b20', 'dht22', 'pir', 'distance', 'pulses', 'aht20', 'bh1750'];
    add(new Select(dom, 'sensor', sensors, sensors, sensors.indexOf(v.sensor)));
    const acts = ['relay', 'fan', 'servo', 'strip'];
    add(new Select(dom, 'actuator', acts, ['Relay', 'Fan', 'Servo', 'Strip'], acts.indexOf(v.actuator)));
    add(new Select(dom, 'unit', ['0', '1', '2'], ['', '', ''], +v.unit));
    for (const n of ['metric', 'above', 'linear', 'toggle']) add(new Select(dom, n, ['0', '1'], ['0', '1'], +v[n]));
    for (const n of ['threshold', 'margin', 'threshold_hi', 'win_start', 'win_end', 'sensor_on'])
      add(new El(dom, 'input', { name: n, value: v[n] }));
    for (const n of ['rule', 'window']) {
      const e = add(new El(dom, 'input', { type: 'checkbox', name: n }));
      e.checked = v[n];
    }
  };
}

const visible = (p) => IDS.filter((i) => !p.$(i).hidden && !['sh', 'ah', 'rs'].includes(i)).join(' ');
const units = (p) => p.dom.names.unit.options.map((o) => (o.hidden ? '-' : o.text)).join('|');

test('form: the parts for the chosen role', () => {
  assert.equal(visible(page({ form: form({ role: 'n' }) })), 'su al rn rt');
  assert.equal(visible(page({ form: form({ role: 's' }) })), 'fs su al rn rt');
  assert.equal(visible(page({ form: form({ role: 'a' }) })), 'fa su al rn rt');
  assert.equal(visible(page({ form: form({}) })), 'fs fa fr su al rn rt');
  // No role checked yet counts as nothing
  const p = page({ form: form({ role: 'x' }) });
  assert.equal(visible(p), 'su al rn rt');
});

test('form: the unit options by sensor, and what each unit asks for', () => {
  let p = page({ form: form({ sensor: 'ds18b20' }) });
  assert.equal(units(p), '°C|°F|-');
  assert.equal(p.dom.names.unit.options[2].disabled, true);
  p = page({ form: form({ sensor: 'dht22', metric: '1' }) });
  assert.equal(units(p), '°C|°F|-');
  assert.match(visible(p), /sm su/);
  p = page({ form: form({ sensor: 'distance', unit: '0' }) });
  assert.equal(units(p), 'Distance (cm)|Tank level (%)|Tank volume (L)');
  assert.doesNotMatch(visible(p), /st|sl/);
  p = page({ form: form({ sensor: 'distance', unit: '1' }) });
  assert.match(visible(p), /su st/);
  assert.doesNotMatch(visible(p), /sl/);
  p = page({ form: form({ sensor: 'distance', unit: '2' }) });
  assert.match(visible(p), /su st sl/);
  p = page({ form: form({ sensor: 'pulses', unit: '0' }) });
  assert.equal(units(p), 'Pulses per 10 s|Flow (L/min)|Flow (mL/min)');
  assert.doesNotMatch(visible(p), /sq/);
  p = page({ form: form({ sensor: 'pulses', unit: '2' }) });
  assert.match(visible(p), / sq /);
  assert.equal(p.$('st').hidden, true);                  // the tank (and its litres, inside) stays hidden
  // An AHT20 has °C and °F; the light has no unit
  p = page({ form: form({ sensor: 'aht20' }) });
  assert.equal(units(p), '°C|°F|-');
  assert.match(visible(p), /sm su/);
  p = page({ form: form({ sensor: 'bh1750', threshold: '50', above: '0', margin: '0' }) });
  assert.doesNotMatch(visible(p), /su/);
  assert.match(p.$('rs').textContent, /below 50 lx/);
  // A digital sensor has no unit
  p = page({ form: form({ sensor: 'pir' }) });
  assert.match(visible(p), /sd/);
  assert.doesNotMatch(visible(p), /su/);
  assert.equal(units(p), '-|-|-');
  // From a tank in litres to a thermometer: the third option is gone, back to the first
  p = page({ form: form({ sensor: 'distance', unit: '2' }) });
  p.dom.names.sensor.value = 'ds18b20';
  p.dom.names.sensor.fire('input');
  assert.equal(p.dom.names.unit.value, '0');
  assert.equal(units(p), '°C|°F|-');
});

test('form: the rule sentence in the unit set', () => {
  const s = (v) => page({ form: form(v) }).$('rs').textContent;
  assert.equal(s({ sensor: 'ds18b20', unit: '1', threshold: '80', margin: '2', above: '1' }),
    'Turns the relay on when the value goes above 80 °F and off when it goes back below 78 °F.');
  assert.equal(s({ sensor: 'ds18b20', unit: '0', above: '0', threshold: '5', margin: '1' }),
    'Turns the relay on when the value goes below 5 °C and off when it goes back above 6 °C.');
  assert.equal(s({ sensor: 'dht22', metric: '0', threshold: '60', margin: '5' }),
    'Turns the relay on when the value goes above 60 % and off when it goes back below 55 %.');
  assert.match(s({ sensor: 'distance', unit: '1', threshold: '20', above: '0', margin: '0' }), /below 20 % and off when it goes back above 20 %/);
  assert.match(s({ sensor: 'distance', unit: '2', threshold: '300' }), /300 L/);
  assert.match(s({ sensor: 'distance', unit: '0', threshold: '30' }), /30 cm/);
  assert.match(s({ sensor: 'pulses', unit: '1', threshold: '2' }), /2 L\/min/);
  assert.match(s({ sensor: 'pulses', unit: '0', threshold: '9' }), /9 pulses/);
  assert.equal(s({ actuator: 'fan', linear: '1', threshold: '25', threshold_hi: '35', unit: '1' }),
    'Level proportional from 25 to 35 °F.');
  assert.equal(s({ rule: false }), 'No rule: the actuator only obeys commands.');
  assert.equal(s({ sensor: 'pir', sensor_on: '', toggle: '0' }), 'Turns the relay on while the sensor is active.');
  assert.equal(s({ sensor: 'pir', sensor_on: 'motion', toggle: '1' }), 'Each time the sensor becomes motion, the relay flips.');
  assert.match(s({ window: true }), / Only between 22:00 and 06:00 \(local time\); off outside it\.$/);
});

test('form: the parts for each actuator, the wiring hints, and every change redraws', () => {
  const vis = (v) => visible(page({ form: form(v) }));
  assert.match(vis({ actuator: 'relay' }), / al /);
  assert.match(vis({ actuator: 'fan' }), / ap rn rl rt$/);
  assert.match(vis({ actuator: 'fan', linear: '1' }), / ap rn rl rh rt$/);
  assert.match(vis({ actuator: 'servo' }), / aa /);
  assert.match(vis({ actuator: 'strip' }), / ac /);
  assert.match(vis({ window: true }), / rt rw$/);
  assert.match(vis({ sensor: 'pir' }), / rd rt$/);
  const p = page({ form: form({ sensor: 'dht22', actuator: 'servo' }) });
  assert.match(p.$('sh').textContent, /^DHT11\/DHT22: 4\.7 to 10 kOhm.* GPIO0 and GPIO2 must be high at boot/);
  // The I2C sensors: one hint, with each address
  const hint = (s) => page({ form: form({ sensor: s }) }).$('sh').textContent;
  assert.match(hint('aht20'), /^I2C: SDA on this pin.*; address 0x38\. GPIO0/);
  assert.match(hint('bh1750'), /address 0x23\./);
  assert.match(p.$('ah').textContent, /^Servo on a separate 5 V supply/);
  // Every field redraws on input
  assert.equal(p.dom.inputs.every((e) => (e.listeners.input || []).length === 1), true);
  p.dom.names.rule.checked = false;
  p.dom.names.rule.fire('input');
  assert.equal(p.$('rs').textContent, 'No rule: the actuator only obeys commands.');
});
