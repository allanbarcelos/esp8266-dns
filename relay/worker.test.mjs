// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Tests of the relay Worker on Node (node --test), with the Workers runtime
// pieces it uses imitated here: WebSocketPair, the hibernation API of the
// Durable Object context, its storage and alarms, and a 101 Response.
import { test, beforeEach, afterEach, mock } from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { pathToFileURL } from 'node:url';

class FakeWS {
  constructor() {
    this.sent = [];
    this.closed = null;
    this.broken = false;
  }
  send(d) {
    if (this.broken) throw new Error('closed');
    this.sent.push(d);
  }
  close(code, reason) {
    this.closed = [code, reason];
  }
}
globalThis.WebSocketPair = class {
  constructor() {
    this[0] = new FakeWS();
    this[1] = new FakeWS();
  }
};
globalThis.WebSocketRequestResponsePair = class {
  constructor(request, response) {
    this.request = request;
    this.response = response;
  }
};
const RealResponse = Response;
globalThis.Response = class extends RealResponse {
  constructor(body, init = {}) {
    if (init.status === 101) {
      super(null, { status: 200 });
      Object.defineProperty(this, 'status', { value: 101 });
      this.webSocket = init.webSocket;
    } else {
      super(body, init);
    }
  }
};

// WORKER: another file to test, such as the minified script the firmware embeds
const worker = await import(process.env.WORKER ? pathToFileURL(process.env.WORKER).href : './worker.mjs');
const { Device, same, ipBytes, hmacHex, toResponse } = worker;
const SECRET = 'ab'.repeat(32);
const enc = new TextEncoder();
const dec = new TextDecoder();

function makeCtx() {
  const store = new Map();
  const ctx = {
    sockets: [],
    auto: null,
    alarm: null,
    store,
    setWebSocketAutoResponse(p) {
      this.auto = p;
    },
    acceptWebSocket(ws) {
      this.sockets.push(ws);
    },
    getWebSockets() {
      return this.sockets.filter((w) => !w.closed);
    },
    storage: {
      get: async (k) => store.get(k),
      put: async (k, v) => {
        store.set(k, v);
      },
      delete: async (k) => {
        store.delete(k);
      },
      setAlarm: async (t) => {
        ctx.alarm = t;
      },
      deleteAlarm: async () => {
        ctx.alarm = null;
      },
    },
  };
  return ctx;
}

let ctx, device, env;
beforeEach(() => {
  mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 1_700_000_000_000 });
  ctx = makeCtx();
  env = { SECRET };
  device = new Device(ctx, env);
  env.DEVICE = { idFromName: (n) => n, get: () => device };
});
afterEach(() => {
  mock.timers.reset();
  mock.restoreAll();
});

const flush = async () => {
  for (let i = 0; i < 20; i++) await Promise.resolve();
};

// The device connects; returns its end of the socket
async function connect() {
  const res = await device.fetch(new Request('https://r.example/.relay'));
  assert.equal(res.status, 101);
  return ctx.sockets.at(-1);
}

// The device answers the next request frame with raw HTTP, in pieces
async function answer(ws, raw, pieces = 1) {
  const f = ws.sent.at(-1);
  const id = [f[1], f[2]];
  const b = enc.encode(raw);
  const step = Math.ceil(b.length / pieces);
  for (let i = 0; i < b.length; i += step) {
    await device.webSocketMessage(ws, new Uint8Array([68, ...id, ...b.subarray(i, i + step)]).buffer);
  }
  await device.webSocketMessage(ws, new Uint8Array([69, ...id]).buffer);
}

const msg = (type, s) => new Uint8Array([type.charCodeAt(0), ...enc.encode(s)]).buffer;

// ── helpers ──────────────────────────────────────────────────────────────────

test('same compares whole strings', () => {
  assert.ok(same('abc', 'abc'));
  assert.ok(!same('abc', 'abd'));
  assert.ok(!same('abc', 'ab'));
  assert.ok(!same('a', ''));
  assert.ok(same('', ''));
});

test('ipBytes keeps IPv4 and hashes the rest', () => {
  assert.deepEqual(ipBytes('203.0.113.9'), [203, 0, 113, 9]);
  const v6 = ipBytes('2001:db8::1');
  assert.equal(v6.length, 4);
  assert.deepEqual(v6, ipBytes('2001:db8::1'));
  assert.notDeepEqual(v6, ipBytes('2001:db8::2'));
  assert.ok(v6.every((b) => b >= 0 && b <= 255));
});

test('hmacHex matches HMAC-SHA1', async () => {
  assert.equal(await hmacHex('key', 'token'), createHmac('sha1', 'key').update('token').digest('hex'));
});

test('toResponse keeps status, headers and body', async () => {
  const r = toResponse(enc.encode('HTTP/1.1 302 \r\nLocation: /login\r\nConnection: close\r\nContent-Length: 2\r\nnoise\r\nSet-Cookie: sid=1\r\n\r\nok'));
  assert.equal(r.status, 302);
  assert.equal(r.headers.get('Location'), '/login');
  assert.equal(r.headers.get('Set-Cookie'), 'sid=1; Secure');            // HTTPS only, here
  assert.equal(r.headers.get('Strict-Transport-Security'), 'max-age=31536000');
  assert.equal(r.headers.get('Connection'), null);
  assert.equal(r.headers.get('Content-Length'), null);
  assert.equal(await r.text(), 'ok');
});

// A gzipped body (the panel's CSS/JS) must not be gzipped again by the Worker
test('toResponse sends the body as encoded by the device', async () => {
  const Real = globalThis.Response;
  let init;
  globalThis.Response = class extends Real {
    constructor(body, i) {
      super(body, i);
      init = i;
    }
  };
  try {
    const gz = new Uint8Array([0x1f, 0x8b, 8, 0]);
    const r = toResponse(new Uint8Array([...enc.encode('HTTP/1.1 200 \r\nContent-Encoding: gzip\r\n\r\n'), ...gz]));
    assert.equal(init.encodeBody, 'manual');
    assert.equal(r.headers.get('Content-Encoding'), 'gzip');
    assert.deepEqual(new Uint8Array(await r.arrayBuffer()), gz);
  } finally {
    globalThis.Response = Real;
  }
});

test('toResponse refuses a broken answer', () => {
  assert.equal(toResponse(enc.encode('HTTP/1.1 200 \r\n')).status, 502);
  assert.equal(toResponse(enc.encode('HTTP/1.1 abc \r\n\r\n')).status, 502);
  assert.equal(toResponse(enc.encode('HTTP/1.1 101 \r\n\r\n')).status, 502);
  assert.equal(toResponse(enc.encode('HTTP/1.1 700 \r\n\r\n')).status, 502);
  assert.equal(toResponse(enc.encode('HTTP/1.1 204 \r\n\r\nbody')).status, 502);
});

// ── the Worker in front ──────────────────────────────────────────────────────

test('relay connection needs WebSocket, version and secret', async () => {
  const h = (extra) => ({ headers: { Upgrade: 'websocket', 'X-Relay-Version': '6', Authorization: `Bearer ${SECRET}`, ...extra } });
  const go = (init) => worker.default.fetch(new Request('https://r.example/.relay', init), env);
  assert.equal((await go({})).status, 426);
  assert.equal((await go(h({ 'X-Relay-Version': '5' }))).status, 426);
  assert.equal((await go(h({ Authorization: 'Bearer nope' }))).status, 401);
  const noAuth = h();
  delete noAuth.headers.Authorization;
  assert.equal((await go(noAuth)).status, 401);
  assert.equal((await go(h())).status, 101);
  assert.equal(ctx.auto.request, 'ping');
  assert.equal(ctx.auto.response, 'pong');
});

test('plain HTTP goes to HTTPS, before anything else', async () => {
  const r = await worker.default.fetch(new Request('http://r.example/login?x=1', { method: 'POST' }), env);
  assert.equal(r.status, 301);
  assert.equal(r.headers.get('Location'), 'https://r.example/login?x=1');
  assert.equal(ctx.sockets.length, 0);
});

test('other paths go to the device', async () => {
  const ws = await connect();
  const res = worker.default.fetch(new Request('https://r.example/status'), env);
  await flush();
  await answer(ws, 'HTTP/1.1 200 \r\n\r\nhi');
  assert.equal(await (await res).text(), 'hi');
});

// ── requests ─────────────────────────────────────────────────────────────────

test('a request goes down as an R frame and the answer comes back', async () => {
  const ws = await connect();
  const req = new Request('https://esp.example.com/login?next=%2F', {
    method: 'POST',
    headers: {
      'CF-Connecting-IP': '198.51.100.7',
      Authorization: 'Bearer t',
      Cookie: 'sid=x',
      'Content-Type': 'application/x-www-form-urlencoded',
      'X-Other': 'dropped',
    },
    body: 'user=a&pass=b',
  });
  const res = device.fetch(req);
  await flush();
  const f = ws.sent.at(-1);
  assert.equal(f[0], 82);
  assert.deepEqual([...f.subarray(1, 3)], [0, 1]);
  assert.deepEqual([...f.subarray(3, 7)], [198, 51, 100, 7]);
  assert.equal(
    dec.decode(f.subarray(7)),
    'POST /login?next=%2F HTTP/1.1\r\nHost: esp.example.com\r\nAuthorization: Bearer t\r\nCookie: sid=x\r\n' +
      'Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 13\r\n\r\nuser=a&pass=b',
  );
  await answer(ws, 'HTTP/1.1 200 \r\nContent-Type: text/html\r\n\r\n<p>panel</p>', 3);
  const r = await res;
  assert.equal(r.status, 200);
  assert.equal(r.headers.get('Content-Type'), 'text/html');
  assert.equal(await r.text(), '<p>panel</p>');
});

test('requests take turns, ids wrap', async () => {
  const ws = await connect();
  device.id = 65534;
  const a = device.fetch(new Request('https://r.example/a'));
  const b = device.fetch(new Request('https://r.example/b'));
  await flush();
  assert.equal(ws.sent.length, 1);
  assert.deepEqual([...ws.sent[0].subarray(1, 3)], [255, 255]);
  await answer(ws, 'HTTP/1.1 200 \r\n\r\na');
  await flush();
  assert.equal(ws.sent.length, 2);
  assert.deepEqual([...ws.sent[1].subarray(1, 3)], [0, 1]);
  assert.ok(!dec.decode(ws.sent[1]).includes('Content-Length'));
  await answer(ws, 'HTTP/1.1 200 \r\n\r\nb');
  assert.equal(await (await a).text(), 'a');
  assert.equal(await (await b).text(), 'b');
});

test('a request waits for the device to connect', async () => {
  const res = device.fetch(new Request('https://r.example/x'));
  await flush();
  mock.timers.tick(3000);
  const ws = await connect();
  await flush();
  await answer(ws, 'HTTP/1.1 200 \r\n\r\nlate');
  assert.equal(await (await res).text(), 'late');
});

test('device offline', async () => {
  const res = device.fetch(new Request('https://r.example/x'));
  await flush();
  mock.timers.tick(8000);
  const r = await res;
  assert.equal(r.status, 503);
  assert.equal(await r.text(), 'Device offline');
});

test('offline /api/state answers from the last state to the token holder', async () => {
  const token = '0123456789abcdef0123456789abcdef';
  const ws = await connect();
  await device.webSocketMessage(ws, msg('H', JSON.stringify({ v: 1, api: await hmacHex(SECRET, token) })));
  await device.webSocketMessage(ws, msg('S', '{"api":1,"sensor":null}'));
  ws.closed = [1006, ''];

  const ask = async (init, path = '/api/state') => {
    const res = device.fetch(new Request(`https://r.example${path}`, init));
    await flush();
    mock.timers.tick(8000);
    return res;
  };
  const ok = await ask({ headers: { Authorization: `Bearer ${token}` } });
  assert.equal(ok.status, 200);
  assert.equal(ok.headers.get('Cache-Control'), 'no-store');
  assert.deepEqual(await ok.json(), { api: 1, sensor: null, online: false, seen: 1_700_000_000 });

  assert.equal((await ask({ headers: { Authorization: 'Bearer wrong' } })).status, 503);
  assert.equal((await ask({ headers: { Authorization: `Basic ${token}` } })).status, 503);
  assert.equal((await ask({})).status, 503);
  assert.equal((await ask({ method: 'POST', headers: { Authorization: `Bearer ${token}` } })).status, 503);
  assert.equal((await ask({ headers: { Authorization: `Bearer ${token}` } }, '/status')).status, 503);

  ctx.store.set('hello', { v: 1, api: '' });
  assert.equal((await ask({ headers: { Authorization: `Bearer ${token}` } })).status, 503);
  ctx.store.delete('hello');
  assert.equal((await ask({ headers: { Authorization: `Bearer ${token}` } })).status, 503);
  ctx.store.set('hello', { v: 1, api: await hmacHex(SECRET, token) });
  ctx.store.delete('state');
  assert.equal((await ask({ headers: { Authorization: `Bearer ${token}` } })).status, 503);
});

test('a broken stored state fails that request, not the queue', async () => {
  const token = 't';
  ctx.store.set('hello', { api: await hmacHex(SECRET, token) });
  ctx.store.set('state', { json: '{broken', at: 0 });
  const bad = device.fetch(new Request('https://r.example/api/state', { headers: { Authorization: `Bearer ${token}` } }));
  await flush();
  mock.timers.tick(8000);
  await assert.rejects(bad);
  const next = device.fetch(new Request('https://r.example/x'));
  await flush();
  mock.timers.tick(8000);
  assert.equal((await next).status, 503);
});

test('sign-in and API requests are limited per address', async () => {
  const ws = await connect();
  const hit = async (path, ip = '192.0.2.1') => {
    const res = device.fetch(new Request(`https://r.example${path}`, { headers: { 'CF-Connecting-IP': ip } }));
    await flush();
    if (ws.sent.length && ws.calls !== ws.sent.length) {
      ws.calls = ws.sent.length;
      await answer(ws, 'HTTP/1.1 200 \r\n\r\n');
    }
    return (await res).status;
  };
  for (let i = 0; i < 60; i++) assert.equal(await hit(i % 2 ? '/api/state' : '/login'), 200);
  assert.equal(await hit('/mfa'), 429);
  // Everything else that checks the password or a code counts too
  for (const p of ['/mfa/reset', '/password/save', '/iot/clear']) assert.equal(await hit(p), 429);
  assert.equal(await hit('/status'), 200);            // the rest of the panel is not counted
  assert.equal(await hit('/iot/clearx'), 200);
  assert.equal(await hit('/api/cmd', '192.0.2.2'), 200);
  const anon = device.fetch(new Request('https://r.example/login'));   // no address header
  await flush();
  ws.calls = ws.sent.length;
  await answer(ws, 'HTTP/1.1 200 \r\n\r\n');
  assert.equal((await anon).status, 200);
  mock.timers.tick(60000);
  assert.equal(await hit('/login'), 200);
});

test('too large, or too many waiting', async () => {
  const big = device.fetch(new Request('https://r.example/save', { method: 'POST', body: 'x'.repeat(2049) }));
  assert.equal((await big).status, 413);
  const ws = await connect();
  const waiting = [];
  for (let i = 0; i < 16; i++) waiting.push(device.fetch(new Request(`https://r.example/${i}`)));
  await flush();
  assert.equal((await device.fetch(new Request('https://r.example/17'))).status, 503);
  for (let i = 0; i < 16; i++) {
    await answer(ws, 'HTTP/1.1 200 \r\n\r\n');
    await flush();
  }
  for (const w of waiting) assert.equal((await w).status, 200);
  assert.equal(device.queued, 0);
});

test('no answer in time', async () => {
  await connect();
  const res = device.fetch(new Request('https://r.example/x'));
  await flush();
  mock.timers.tick(20000);
  assert.equal((await res).status, 504);
});

test('the device goes away mid-request', async () => {
  const ws = await connect();
  const a = device.fetch(new Request('https://r.example/a'));
  await flush();
  device.webSocketClose(ws, 1006, '', false);
  assert.equal((await a).status, 502);
  const b = device.fetch(new Request('https://r.example/b'));
  await flush();
  device.webSocketError(ws, new Error('x'));
  assert.equal((await b).status, 502);
  ws.broken = true;
  assert.equal((await device.fetch(new Request('https://r.example/c'))).status, 502);
});

test('frames the Worker does not expect are ignored', async () => {
  const ws = await connect();
  await device.webSocketMessage(ws, 'pong');
  await device.webSocketMessage(ws, new Uint8Array([68, 0, 9, 65]).buffer);
  await device.webSocketMessage(ws, new Uint8Array([69, 0, 9]).buffer);
  await device.webSocketMessage(ws, msg('Z', 'x'));
  assert.equal(ctx.store.size, 0);
});

test('a new connection replaces the old one', async () => {
  const a = await connect();
  const b = await connect();
  assert.deepEqual(a.closed, [1000, 'replaced']);
  assert.equal(b.closed, null);
  assert.deepEqual(ctx.getWebSockets(), [b]);
});

// ── hello, state and ntfy ────────────────────────────────────────────────────

test('state and hello are kept; hello sets where ntfy resumes', async () => {
  const ws = await connect();
  await device.webSocketMessage(ws, msg('S', '{"api":1}'));
  assert.deepEqual(ctx.store.get('state'), { json: '{"api":1}', at: 1_700_000_000_000 });

  await device.webSocketMessage(ws, msg('H', '{"v":6,"topic":"cmd-x","poll":5}'));
  assert.equal(ctx.store.get('since'), '1700000000');           // from now on
  assert.equal(ctx.alarm, 1_700_000_001_000);
  await device.webSocketMessage(ws, msg('H', '{"v":6,"topic":"cmd-x"}'));
  assert.equal(ctx.store.get('since'), '1700000000');           // kept
  await device.webSocketMessage(ws, msg('H', '{"v":6,"topic":"cmd-x","since":"abc123"}'));
  assert.equal(ctx.store.get('since'), 'abc123');               // the device knows best
  await device.webSocketMessage(ws, msg('H', '{"v":6,"topic":"cmd-x"}'));
  assert.equal(ctx.store.get('since'), 'abc123');               // same server and topic: kept
  await device.webSocketMessage(ws, msg('H', '{"v":6,"topic":"cmd-y"}'));
  assert.equal(ctx.store.get('since'), '1700000000');           // another topic: from now on
  ctx.store.set('since', 'abc123');
  await device.webSocketMessage(ws, msg('H', '{"v":6,"host":"ntfy.example.com","topic":"cmd-y"}'));
  assert.equal(ctx.store.get('since'), '1700000000');           // another server: from now on
  ctx.store.set('release', 1);
  await device.webSocketMessage(ws, msg('H', '{"v":6}'));
  assert.equal(ctx.alarm, 1_700_000_001_000);                  // the release check, even without commands
  assert.equal(ctx.store.has('release'), false);                // each connection checks it first
});

test('alarm polls ntfy and forwards the messages', async () => {
  const ws = await connect();
  ctx.store.set('hello', { topic: 'cmd x', token: 'tk_1', poll: 10 });
  ctx.store.set('since', 'old');
  ctx.store.set('release', Infinity);                           // checked already
  const calls = [];
  mock.method(globalThis, 'fetch', async (url, init) => {
    calls.push([url, init]);
    const lines = [
      '{"event":"open"}',
      'junk',
      '{"event":"message"}',
      '{"event":"message","id":"m1","time":1,"message":"on","topic":"cmd x"}',
      '{"event":"message","id":"m2","time":2,"message":"off"}',
      '',
    ];
    return new RealResponse(lines.join('\n'));
  });
  await device.alarm();
  assert.equal(calls[0][0], 'https://ntfy.sh/cmd%20x/json?poll=1&since=old');
  assert.equal(calls[0][1].headers.Authorization, 'Bearer tk_1');
  assert.equal(ws.sent.length, 2);
  assert.equal(ws.sent[0][0], 67);
  assert.deepEqual(JSON.parse(dec.decode(ws.sent[0].subarray(1))), { event: 'message', id: 'm1', time: 1, message: 'on' });
  assert.equal(ctx.store.get('since'), 'm2');
  assert.equal(ctx.alarm, 1_700_000_010_000);
});

test('alarm on the own server, without token, with nothing new, or failing', async () => {
  const ws = await connect();
  ctx.store.set('hello', { host: 'ntfy.example.com', topic: 'cmd', poll: 7 });
  ctx.store.set('since', 's');
  ctx.store.set('release', Infinity);
  const calls = [];
  mock.method(globalThis, 'fetch', async (url, init) => {
    calls.push([url, init]);
    return new RealResponse('', { status: 200 });
  });
  await device.alarm();
  assert.equal(calls[0][0], 'https://ntfy.example.com/cmd/json?poll=1&since=s');
  assert.deepEqual(calls[0][1].headers, {});
  assert.equal(ctx.alarm, 1_700_000_005_000);                   // 7 is not offered: 5
  assert.equal(ctx.store.get('since'), 's');

  mock.method(globalThis, 'fetch', async () => new RealResponse('{"event":"message","id":"x"}', { status: 500 }));
  await device.alarm();
  assert.equal(ctx.store.get('since'), 's');
  assert.equal(ws.sent.length, 0);

  mock.method(globalThis, 'fetch', async () => {
    throw new Error('down');
  });
  ctx.alarm = null;
  await device.alarm();
  assert.equal(ctx.alarm, 1_700_000_005_000);
});

test('alarm stops without the device or its hello; without a topic, only the release', async () => {
  const urls = [];
  mock.method(globalThis, 'fetch', async (url) => {
    urls.push(url);
    return new RealResponse('', { status: 404 });
  });
  ctx.store.set('hello', { topic: 'cmd' });
  await device.alarm();                       // no device connected
  await connect();
  ctx.store.delete('hello');
  await device.alarm();                       // no hello yet
  assert.equal(urls.length, 0);
  assert.equal(ctx.alarm, null);
  ctx.store.set('hello', {});
  await device.alarm();                       // no topic: the release, then in an hour
  assert.deepEqual(urls, ['https://api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest']);
  assert.equal(ctx.alarm, 1_700_003_600_000);
});

// ── the rest of the device's network ─────────────────────────────────────────

test('the device is told the address it connects from', async () => {
  const res = await device.fetch(new Request('https://r.example/.relay', { headers: { 'CF-Connecting-IP': '203.0.113.7' } }));
  assert.equal(res.status, 101);
  const ws = ctx.sockets.at(-1);
  assert.equal(ws.sent[0][0], 73);
  assert.equal(dec.decode(ws.sent[0].subarray(1)), '203.0.113.7');
});

test('the device\'s ntfy posts go out from here', async () => {
  const ws = await connect();
  const calls = [];
  mock.method(globalThis, 'fetch', async (url, init) => {
    calls.push([url, init]);
    return new RealResponse('', { status: 200 });
  });
  const head = 'Content-Type: text/plain\r\nAuthorization: Bearer tk_1\r\nActions: http, On, https://ntfy.sh/c, method=POST, body=on\r\nnoise\r\n';
  await device.webSocketMessage(ws, msg('P', ['ntfy.sh', 'data t', head, '[sensor] 21.0 C | on'].join('\0')));
  assert.equal(calls[0][0], 'https://ntfy.sh/data%20t');
  assert.equal(calls[0][1].method, 'POST');
  assert.equal(calls[0][1].body, '[sensor] 21.0 C | on');
  assert.deepEqual(calls[0][1].headers, {
    'Content-Type': 'text/plain',
    Authorization: 'Bearer tk_1',
    Actions: 'http, On, https://ntfy.sh/c, method=POST, body=on',
  });
  // An unreachable server loses that message, nothing else
  mock.method(globalThis, 'fetch', async () => {
    throw new Error('down');
  });
  await device.webSocketMessage(ws, msg('P', ['ntfy.example.com', 't', '', 'x'].join('\0')));
  assert.equal(ws.sent.length, 0);
});

test('the latest release goes down once an hour', async () => {
  const ws = await connect();
  ctx.store.set('hello', {});
  let answer = () => new RealResponse(JSON.stringify({ tag_name: 'v158' }));
  const calls = [];
  mock.method(globalThis, 'fetch', async (url, init) => {
    calls.push([url, init]);
    return answer();
  });
  await device.alarm();
  assert.equal(calls[0][1].headers['User-Agent'], 'esp8266-dns-relay');
  assert.equal(ws.sent.at(-1)[0], 86);
  assert.equal(dec.decode(ws.sent.at(-1).subarray(1)), 'v158');
  assert.equal(ctx.store.get('release'), 1_700_003_600_000);
  await device.alarm();                                         // within the hour: no new check
  assert.equal(calls.length, 1);
  // Anything but a "v" and digits, a refusal or no answer: nothing goes down
  const sent = ws.sent.length;
  for (const a of [() => new RealResponse(JSON.stringify({ tag_name: 'latest' })),
    () => new RealResponse(JSON.stringify({})), () => new RealResponse('limit', { status: 403 }),
    () => { throw new Error('down'); }]) {
    answer = a;
    ctx.store.delete('release');
    await device.alarm();
  }
  assert.equal(ws.sent.length, sent);
});
