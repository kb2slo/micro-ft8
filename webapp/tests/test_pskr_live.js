#!/usr/bin/env node
// pskr_live.js (docs/architecture/webapp.md, Who hears me): the listener app.html and map.html share, driven with a
// fake WebSocket, clock, timers and localStorage -- when it connects and stops, the MQTT exchange, a dead socket,
// back-off, the stored copy both pages read, and a second tab's spots merged in.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

let clock = Date.UTC(2026, 9, 8, 15, 0, 0);
class FakeDate extends Date {
  constructor(...a) { if (a.length) super(...a); else super(clock); }
  static now() { return clock; }
}
let timers = [], nextId = 1;
const fakeSetTimeout = (fn, ms) => { const id = nextId++; timers.push({ id, fn, at: clock + ms, every: 0 }); return id; };
const fakeSetInterval = (fn, ms) => { const id = nextId++; timers.push({ id, fn, at: clock + ms, every: ms }); return id; };
const fakeClear = (id) => { timers = timers.filter((t) => t.id !== id); };
function advance(ms) {
  const end = clock + ms;
  for (;;) {
    timers.sort((a, b) => a.at - b.at);
    const t = timers[0];
    if (!t || t.at > end) break;
    clock = t.at;
    if (t.every) t.at += t.every; else timers.shift();
    t.fn();
  }
  clock = end;
}

const sockets = [];
class FakeWS {
  constructor(url, proto) { Object.assign(this, { url, proto, sent: [], readyState: 0, closed: false }); sockets.push(this); }
  send(b) { this.sent.push(Array.from(b)); }
  close() { this.closed = true; this.readyState = 3; }
  open() { this.readyState = 1; this.onopen(); }
  deliver(bytes) { this.onmessage({ data: Uint8Array.from(bytes).buffer }); }
  lost() { this.readyState = 3; this.onclose(); }
}
const store = new Map();
const localStorage = { getItem: (k) => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)) };
const docListeners = {}, winListeners = {};
const navigator = { onLine: true };

const ctx = vm.createContext({
  TextEncoder, TextDecoder, Date: FakeDate, WebSocket: FakeWS, localStorage, navigator,
  setTimeout: fakeSetTimeout, setInterval: fakeSetInterval, clearTimeout: fakeClear, clearInterval: fakeClear,
  document: { visibilityState: 'visible', addEventListener: (t, f) => { docListeners[t] = f; } },
  window: { addEventListener: (t, f) => { winListeners[t] = f; } }
});
for (const f of ['pskr_core.js', 'pskr_live.js']) {
  vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', f), 'utf8'), ctx);
}
const P = vm.runInContext('PskrCore', ctx);
const L = vm.runInContext('PskrLive', ctx);

let failed = false;
function check(ok, msg) { if (!ok) { console.error('FAIL: ' + msg); failed = true; } }
function same(a, b, msg) {
  check(JSON.stringify(a) === JSON.stringify(b), msg + ' (got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b) + ')');
}

function publish(payload) {
  const t = Buffer.from('pskr/filter/v2/20m/FT8/KB2SLO/K1ABC'), p = Buffer.from(payload);
  const body = [t.length >> 8, t.length & 255, ...t, ...p];
  const len = [];
  let n = body.length;
  do { const d = n % 128; n = Math.floor(n / 128); len.push(n ? d | 128 : d); } while (n);
  return [0x30, ...len, ...body];
}
function spotMsg(rx, sc, tSec) {
  return JSON.stringify({ sc: sc || 'KB2SLO', rc: rx, rl: 'FN42', b: '20m', md: 'FT8', rp: -10, f: 14075000, t_tx: tSec });
}
const CONNACK = [0x20, 2, 0, 0], SUBACK = [0x90, 3, 0, 1, 0];
const live = () => sockets.filter((w) => !w.closed);
function handshake(ws) { ws.open(); ws.deliver(CONNACK); ws.deliver(SUBACK); }

// The copy another page saved is what this one starts from.
store.set(L.KEY, JSON.stringify({ v: 1, call: 'KB2SLO', listen: [], tx: [],
  spots: [{ rx: 'OLD1', grid: 'FN31', band: '40m', mode: 'FT8', snr: -5, hz: 0, t: clock - 60000 }] }));

let call = 'KB2SLO', want = false;
const changes = [];
L.start({ call: () => call, want: () => want, changed: (w) => changes.push(w) });
same(L.state().spots.map((x) => x.rx), ['OLD1'], 'start: loads the stored copy');
same([live().length, L.status()], [0, 'idle'], 'start: no transmission and nothing on screen, no socket');

want = true;
L.tick();
same(live().length, 1, 'want: one socket');
const ws = sockets[0];
same([ws.url, ws.proto], [P.URL, 'mqtt'], 'want: the feed, MQTT subprotocol');
same(L.status(), 'connecting', 'want: connecting');
ws.open();
same(ws.sent[0][0], 0x10, 'open: CONNECT first');
ws.deliver(CONNACK);
same(ws.sent[1], Array.from(P.subscribePacket(1, P.topicFor('KB2SLO'))), 'CONNACK: SUBSCRIBE to our call');
ws.deliver(SUBACK);
same(L.status(), 'listening', 'SUBACK: listening');
same(L.state().listen.length, 1, 'SUBACK: a listening range starts');

changes.length = 0;
ws.deliver(publish(spotMsg('K1ABC', 'KB2SLO', clock / 1000)));
ws.deliver(publish(spotMsg('W9XYZ', 'W1AW', clock / 1000)));
same(L.state().spots.map((x) => x.rx), ['K1ABC', 'OLD1'], 'PUBLISH: our spot kept, another sender refused');
check(changes.includes('spots'), 'PUBLISH: the page is told');

advance(2000);
same(JSON.parse(store.get(L.KEY)).spots.length, 2, 'saved for the other page within seconds');
advance(P.PING_MS - 2000);
same(ws.sent[ws.sent.length - 1], [0xC0, 0], 'PINGREQ at the ping interval');
ws.deliver([0xD0, 0]); // PINGRESP
const aliveAt = clock;
same(L.state().listen[0][1], aliveAt, 'a ping reply moves the listening range on');

// Nothing heard for DEAD_MS (a phone that locked): the next tick drops it, and the range ends when it last heard.
advance(P.DEAD_MS + L.TICK_MS);
check(ws.closed, 'dead: socket dropped');
same(L.status(), 'unreachable', 'dead: says so');
same(L.state().listen[0][1], aliveAt, 'dead: the range ends at the last packet, not at the drop');
const before = sockets.length;
advance(L.TICK_MS * 2 + 1);
same(sockets.length, before + 1, 'back-off: one retry after its wait');
handshake(sockets[sockets.length - 1]);
same(L.status(), 'listening', 'retry: listening again');

// Lost while subscribed: backs off longer each time; a success resets it.
sockets[sockets.length - 1].lost();
same(L.status(), 'unreachable', 'lost: says so');
const n1 = sockets.length;
advance(L.TICK_MS - 1);
same(sockets.length, n1, 'lost: no retry before the back-off');
advance(L.TICK_MS + 1);
same(sockets.length, n1 + 1, 'lost: retried');
handshake(sockets[sockets.length - 1]);

// Nothing wants it: the socket goes and the copy is saved.
want = false;
L.tick();
same([live().length, L.status()], [0, 'idle'], 'unwanted: no socket');

// A transmission wants it for PskrCore.LAG_MS on its own.
L.noteTx();
same(live().length, 1, 'transmission: listening starts at once');
handshake(live()[0]);
advance(P.LAG_MS + L.TICK_MS);
same(live().length, 0, 'transmission: stops after the lag');

// Offline: no attempt, and it says so.
want = true;
navigator.onLine = false;
L.tick();
same([live().length, L.status()], [0, 'offline'], 'offline: no socket');
navigator.onLine = true;
L.tick();
same(live().length, 1, 'back online: connects');
handshake(live()[0]);

// Another tab saved spots this one lacks: merged.
const other = JSON.parse(JSON.stringify(L.state()));
other.spots.push({ rx: 'TAB2', grid: '', band: '20m', mode: 'FT8', snr: -3, hz: 0, t: clock - 1000 });
winListeners.storage({ key: L.KEY, newValue: JSON.stringify(other) });
check(L.state().spots.some((x) => x.rx === 'TAB2'), 'storage: the other tab\'s spot merged');
const cnt = L.state().spots.length;
winListeners.storage({ key: L.KEY, newValue: JSON.stringify(other) });
same(L.state().spots.length, cnt, 'storage: merged once');

// A different call (changed in Settings): a fresh copy, a new subscription.
const oldWs = live()[0];
call = 'W1AW';
L.sync();
check(oldWs.closed, 'call change: old socket dropped');
same([L.state().call, L.state().spots.length], ['W1AW', 0], 'call change: fresh state');
live()[0].open();
live()[0].deliver(CONNACK);
same(live()[0].sent[1], Array.from(P.subscribePacket(1, P.topicFor('W1AW'))), 'call change: subscribes to the new call');

// No call: nothing.
call = '';
L.sync();
same([live().length, L.state(), L.status()], [0, null, 'idle'], 'no call: no socket, no state');

if (failed) process.exit(1);
console.log('test_pskr_live: ok');
