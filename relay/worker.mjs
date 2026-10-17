// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Remote access relay: a Cloudflare Worker with one Durable Object. The
// device keeps a WebSocket open to /.relay (RelayService.h); each request
// from the internet goes down that connection as an 'R' frame and the
// device's answer comes back as 'D' frames closed by an 'E'. The panel and
// the API stay on the device: this file knows nothing about them.
//
// The device deploys this file itself (CloudflareSetup.h) with one secret,
// SECRET, and a Durable Object binding, DEVICE. tools/embed_relay.py puts
// it into the firmware; VERSION must match RelayService::PROTOCOL.
//
// The device has room for one TLS session only, which this connection
// takes. So while it is connected, the Worker does the rest of its network
// for it: it polls the ntfy command topic ('C' frames down), sends its ntfy
// posts ('P' up), tells it the public address it connects from ('I' down)
// and the latest release on GitHub, once an hour ('V' down).

const VERSION = 6;
const WAIT_MS = 8000;        // a request waits this long for the device to (re)connect
const REPLY_MS = 20000;      // and this long for its answer
const MAX_BODY = 2048;       // HttpServer::MAX_BODY
const MAX_QUEUE = 16;
const RATE = 60;             // sign-in and API requests per minute per address
// What checks a password, a code or the API token: counted per address
const LIMITED = /^\/(api\/|login$|mfa(\/reset)?$|password\/save$|iot\/clear$)/;
const POLLS = [2, 5, 10, 30];
const RELEASES = 'https://api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest';
const RELEASE_MS = 3600000;

const enc = new TextEncoder();
const dec = new TextDecoder();

const text = (status, body) =>
  new Response(body, { status, headers: { 'Content-Type': 'text/plain', 'Cache-Control': 'no-store' } });

// Equal strings, in time that does not depend on where they differ
export function same(a, b) {
  let d = a.length ^ b.length;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i % (b.length || 1));
  return d === 0;
}

// The client's address in 4 bytes: IPv4 as is, anything else hashed (FNV-1a),
// so the device's sign-in counters still tell clients apart
export function ipBytes(ip) {
  const m = /^(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(ip);
  if (m) return [1, 2, 3, 4].map((i) => Number(m[i]) & 255);
  let h = 2166136261;
  for (let i = 0; i < ip.length; i++) h = Math.imul(h ^ ip.charCodeAt(i), 16777619) >>> 0;
  return [h >>> 24, (h >>> 16) & 255, (h >>> 8) & 255, h & 255];
}

export async function hmacHex(key, msg) {
  const k = await crypto.subtle.importKey('raw', enc.encode(key), { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
  const sig = new Uint8Array(await crypto.subtle.sign('HMAC', k, enc.encode(msg)));
  return [...sig].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// The device's answer (status line, headers, body) as a Response
export function toResponse(bytes) {
  let end = -1;
  for (let i = 0; i + 3 < bytes.length; i++) {
    if (bytes[i] === 13 && bytes[i + 1] === 10 && bytes[i + 2] === 13 && bytes[i + 3] === 10) {
      end = i;
      break;
    }
  }
  if (end < 0) return text(502, 'Bad answer from the device');
  const lines = dec.decode(bytes.subarray(0, end)).split('\r\n');
  const status = Number(lines[0].split(' ')[1]);
  if (!(status >= 200 && status <= 599)) return text(502, 'Bad answer from the device');
  const headers = new Headers();
  for (const line of lines.slice(1)) {
    const c = line.indexOf(':');
    const name = line.slice(0, c).trim();
    if (c > 0 && !/^(connection|content-length)$/i.test(name)) {
      const v = line.slice(c + 1).trim();
      // Here the panel is on HTTPS: the cookie goes on HTTPS only
      headers.append(name, /^set-cookie$/i.test(name) ? v + '; Secure' : v);
    }
  }
  headers.set('Strict-Transport-Security', 'max-age=31536000');
  // The body goes out as the device sent it: without 'manual', a Worker
  // gzips again a body whose Content-Encoding says gzip (the panel's CSS/JS)
  try {
    return new Response(bytes.subarray(end + 4), { status, headers, encodeBody: 'manual' });
  } catch {
    return text(502, 'Bad answer from the device');   // a body on a 204 or 304
  }
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    // Never in clear over the internet: a password, a code or the cookie
    if (url.protocol === 'http:') {
      url.protocol = 'https:';
      return Response.redirect(url.href, 301);
    }
    if (url.pathname === '/.relay') {
      if (req.headers.get('Upgrade') !== 'websocket') return text(426, 'WebSocket only');
      if (req.headers.get('X-Relay-Version') !== String(VERSION)) return text(426, 'Relay version mismatch');
      if (!same(req.headers.get('Authorization') || '', 'Bearer ' + env.SECRET)) return text(401, 'Wrong secret');
    }
    return env.DEVICE.get(env.DEVICE.idFromName('device')).fetch(req);
  },
};

export class Device {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    this.calls = new Map();
    this.id = 0;
    this.queued = 0;
    this.chain = Promise.resolve();
    this.waiters = [];
    this.minute = 0;
    this.counts = new Map();
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === '/.relay') return this.accept(req);
    const ip = req.headers.get('CF-Connecting-IP') || '';
    if (LIMITED.test(url.pathname) && !this.allow(ip)) return text(429, 'Too many requests');
    const body = new Uint8Array(await req.arrayBuffer());
    if (body.length > MAX_BODY) return text(413, 'Too large');
    if (this.queued >= MAX_QUEUE) return text(503, 'Device busy');
    this.queued++;
    const run = this.chain.then(() => this.forward(req, url, ip, body));
    const free = () => this.queued--;
    this.chain = run.then(free, free);
    return run;
  }

  allow(ip) {
    const minute = Math.floor(Date.now() / 60000);
    if (minute !== this.minute) {
      this.minute = minute;
      this.counts.clear();
    }
    const n = (this.counts.get(ip) || 0) + 1;
    this.counts.set(ip, n);
    return n <= RATE;
  }

  // The device connects: a newer connection replaces an older one. It is
  // told the public address it comes from (no address check of its own).
  accept(req) {
    for (const old of this.ctx.getWebSockets()) old.close(1000, 'replaced');
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1]);
    const ip = req.headers.get('CF-Connecting-IP');
    if (ip) pair[1].send(new Uint8Array([73, ...enc.encode(ip)]));
    for (const w of this.waiters.splice(0)) w(pair[1]);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  socket() {
    const [ws] = this.ctx.getWebSockets();
    if (ws) return Promise.resolve(ws);
    return new Promise((resolve) => {
      this.waiters.push(resolve);
      setTimeout(() => resolve(null), WAIT_MS);
    });
  }

  async forward(req, url, ip, body) {
    const ws = await this.socket();
    if (!ws) return this.offline(req, url);
    this.id = (this.id % 65535) + 1;
    const id = this.id;
    let head = `${req.method} ${url.pathname}${url.search} HTTP/1.1\r\nHost: ${url.host}\r\n`;
    for (const h of ['Authorization', 'Cookie', 'Content-Type']) {
      const v = req.headers.get(h);
      if (v) head += `${h}: ${v}\r\n`;
    }
    if (body.length) head += `Content-Length: ${body.length}\r\n`;
    const h = enc.encode(head + '\r\n');
    const frame = new Uint8Array(7 + h.length + body.length);
    frame.set([82, id >> 8, id & 255, ...ipBytes(ip)]);
    frame.set(h, 7);
    frame.set(body, 7 + h.length);
    return new Promise((resolve) => {
      const parts = [];
      const done = (res) => {
        clearTimeout(timer);
        this.calls.delete(id);
        resolve(res);
      };
      const timer = setTimeout(() => done(text(504, 'The device did not answer')), REPLY_MS);
      this.calls.set(id, { parts, done });
      try {
        ws.send(frame);
      } catch {
        done(text(502, 'The device went away'));
      }
    });
  }

  // With the device away, /api/state answers from the last state it sent,
  // to whoever holds the API token (checked by its keyed hash)
  async offline(req, url) {
    const auth = req.headers.get('Authorization') || '';
    if (req.method === 'GET' && url.pathname === '/api/state' && auth.startsWith('Bearer ')) {
      const hello = await this.ctx.storage.get('hello');
      const last = await this.ctx.storage.get('state');
      if (hello && hello.api && last && same(await hmacHex(this.env.SECRET, auth.slice(7)), hello.api)) {
        const state = JSON.parse(last.json);
        state.online = false;
        state.seen = Math.floor(last.at / 1000);
        return new Response(JSON.stringify(state), {
          headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
        });
      }
    }
    return text(503, 'Device offline');
  }

  async webSocketMessage(ws, msg) {
    if (typeof msg === 'string') return;
    const b = new Uint8Array(msg);
    const type = String.fromCharCode(b[0]);
    if (type === 'D' || type === 'E') {
      const call = this.calls.get((b[1] << 8) | b[2]);
      if (!call) return;
      if (type === 'D') {
        call.parts.push(b.slice(3));
        return;
      }
      const all = new Uint8Array(call.parts.reduce((n, p) => n + p.length, 0));
      let at = 0;
      for (const p of call.parts) {
        all.set(p, at);
        at += p.length;
      }
      call.done(toResponse(all));
    } else if (type === 'S') {
      await this.ctx.storage.put('state', { json: dec.decode(b.subarray(1)), at: Date.now() });
    } else if (type === 'H') {
      await this.hello(JSON.parse(dec.decode(b.subarray(1))));
    } else if (type === 'P') {
      await this.post(dec.decode(b.subarray(1)).split('\0'));
    }
  }

  // An ntfy post from the device: server, topic, header lines, body. Sent
  // once; a server that does not take it loses this one message.
  async post([host, topic, head, body]) {
    const headers = {};
    for (const line of head.split('\r\n')) {
      const c = line.indexOf(':');
      if (c > 0) headers[line.slice(0, c)] = line.slice(c + 1).trim();
    }
    try {
      await fetch(`https://${host}/${encodeURIComponent(topic)}`, { method: 'POST', headers, body });
    } catch {
      // unreachable: lost, like a post that fails on the device
    }
  }

  // The device's settings for the ntfy commands, and where it left off
  async hello(h) {
    const was = await this.ctx.storage.get('hello');
    await this.ctx.storage.put('hello', h);
    if (h.since) await this.ctx.storage.put('since', h.since);
    // Another server or topic: an ID from the old one means nothing there
    else if (!(await this.ctx.storage.get('since')) || (was && (was.host !== h.host || was.topic !== h.topic))) await this.ctx.storage.put('since', String(Math.floor(Date.now() / 1000)));
    // Each connection starts with the release check, then the ntfy commands
    await this.ctx.storage.delete('release');
    await this.ctx.storage.setAlarm(Date.now() + 1000);
  }

  webSocketClose() {
    for (const call of [...this.calls.values()]) call.done(text(502, 'The device went away'));
  }

  webSocketError() {
    this.webSocketClose();
  }

  // While the device is connected: the latest release once an hour, the
  // ntfy commands every poll seconds
  async alarm() {
    const [ws] = this.ctx.getWebSockets();
    const h = await this.ctx.storage.get('hello');
    if (!ws || !h) return;
    const now = Date.now();
    let next = (await this.ctx.storage.get('release')) || 0;
    if (now >= next) {
      await this.release(ws);
      next = now + RELEASE_MS;
      await this.ctx.storage.put('release', next);
    }
    if (h.topic) next = Math.min(next, await this.commands(ws, h));
    await this.ctx.storage.setAlarm(next);
  }

  // The latest release's tag ("v" and digits); on any failure nothing, and
  // the device checks GitHub on its own
  async release(ws) {
    try {
      const r = await fetch(RELEASES, { headers: { 'User-Agent': 'esp8266-dns-relay', Accept: 'application/vnd.github+json' } });
      const tag = r.ok ? (await r.json()).tag_name : '';
      if (/^v\d{1,9}$/.test(tag)) ws.send(new Uint8Array([86, ...enc.encode(tag)]));
    } catch {
      // GitHub unreachable or limited
    }
  }

  // New messages on the command topic go down; returns when to look again
  async commands(ws, h) {
    const since = await this.ctx.storage.get('since');
    try {
      const r = await fetch(`https://${h.host || 'ntfy.sh'}/${encodeURIComponent(h.topic)}/json?poll=1&since=${encodeURIComponent(since)}`, {
        headers: h.token ? { Authorization: `Bearer ${h.token}` } : {},
      });
      let last = since;
      if (r.ok) {
        for (const line of (await r.text()).split('\n')) {
          let m;
          try {
            m = JSON.parse(line);
          } catch {
            continue;
          }
          if (m.event !== 'message' || !m.id) continue;
          const one = { event: 'message', id: m.id, time: m.time, message: m.message };
          ws.send(new Uint8Array([67, ...enc.encode(JSON.stringify(one))]));
          last = m.id;
        }
      }
      if (last !== since) await this.ctx.storage.put('since', last);
    } catch {
      // ntfy unreachable: next round
    }
    const poll = POLLS.includes(h.poll) ? h.poll : 5;
    return Date.now() + poll * 1000;
  }
}
