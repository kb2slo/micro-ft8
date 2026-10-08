#!/usr/bin/env node
// pskr_core.js (docs/architecture/webapp.md, Who hears me): the MQTT packets, the reader, spots, and the gaps the
// card shows, run under node without a page or a socket.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

const ctx = vm.createContext({ TextEncoder, TextDecoder });
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'pskr_core.js'), 'utf8'), ctx);
const P = vm.runInContext('PskrCore', ctx);

let failed = false;
function check(ok, msg) {
  if (!ok) { console.error('FAIL: ' + msg); failed = true; }
}
function same(a, b, msg) {
  check(JSON.stringify(a) === JSON.stringify(b), msg + ' (got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b) + ')');
}
const bytes = (u8) => Array.from(u8);
const MIN = 60000;

// ---- topic ----
same(P.topicFor('kb2slo'), 'pskr/filter/v2/+/+/KB2SLO/#', 'topic: our call as sender, any band/mode/receiver');
same(P.topicFor('F5OYA/P'), 'pskr/filter/v2/+/+/F5OYA.P/#', "topic: the feed writes '/' as '.'");
same(P.topicFor(''), null, 'topic: no call');
same(P.topicFor('K1#'), null, 'topic: a wildcard character is not a call');

// ---- packets, byte for byte (MQTT 3.1.1 §3.1, §3.8, §3.12) ----
same(bytes(P.connectPacket('ab')),
  [0x10, 14, 0, 4, 77, 81, 84, 84, 4, 2, 0, 60, 0, 2, 97, 98],
  'CONNECT: level 4, clean session, keepalive 60, client id');
same(bytes(P.subscribePacket(1, 'a/b')),
  [0x82, 8, 0, 1, 0, 3, 97, 47, 98, 0],
  'SUBSCRIBE: packet id, topic, QoS 0');
same(bytes(P.PINGREQ), [0xC0, 0], 'PINGREQ');
same(bytes(P.DISCONNECT), [0xE0, 0], 'DISCONNECT');
const longTopic = 'x'.repeat(200);
const sub = P.subscribePacket(2, longTopic);
same(bytes(sub.slice(0, 3)), [0x82, 205 % 128 | 128, 1], 'SUBSCRIBE: a two-byte remaining length');

// ---- reader ----
function publish(topic, payload, flags) {
  const t = new TextEncoder().encode(topic), p = new TextEncoder().encode(payload);
  const qos = ((flags || 0) >> 1) & 3;
  const body = [t.length >> 8, t.length & 255, ...t, ...(qos ? [0, 7] : []), ...p];
  const len = [];
  let n = body.length;
  do { let d = n % 128; n = Math.floor(n / 128); len.push(n ? d | 128 : d); } while (n);
  return Uint8Array.from([0x30 | (flags || 0), ...len, ...body]);
}
const connack = Uint8Array.from([0x20, 2, 0, 0]);
const pub1 = publish('t/1', '{"a":1}');
const pub2 = publish('t/2', 'x'.repeat(300)); // remaining length over 127
let r = P.newReader();
let got = P.readPackets(r, Uint8Array.from([...connack, ...pub1, ...pub2]));
same(got.map((p) => p.type), [P.CONNACK, P.PUBLISH, P.PUBLISH], 'reader: three packets in one message');
same(P.publishOf(got[1]), { topic: 't/1', payload: '{"a":1}' }, 'reader: PUBLISH topic and payload');
same(P.publishOf(got[2]).payload.length, 300, 'reader: a long PUBLISH whole');

r = P.newReader();
const all = Uint8Array.from([...pub1, ...pub2]);
got = [];
for (let i = 0; i < all.length; i += 5) got = got.concat(P.readPackets(r, all.slice(i, i + 5)));
same(got.map((p) => P.publishOf(p).topic), ['t/1', 't/2'], 'reader: packets split across many messages');
same(r.buf.length, 0, 'reader: nothing left over');

r = P.newReader();
const pk = publish('q', 'z'.repeat(200), 2); // QoS 1, so a packet id follows the topic; two length bytes
same(P.readPackets(r, pk.slice(0, 1)).length, 0, 'reader: a lone first byte waits');
same(P.readPackets(r, pk.slice(1, 2)).length, 0, 'reader: half a length waits');
same(P.publishOf(P.readPackets(r, pk.slice(2))[0]),
  { topic: 'q', payload: 'z'.repeat(200) }, 'reader: the rest completes it, and the QoS 1 packet id is skipped');
let threw = false;
try { P.readPackets(P.newReader(), Uint8Array.from([0x30, 0xff, 0xff, 0xff, 0xff, 0x01])); } catch (e) { threw = true; }
check(threw, 'reader: a five-byte length is refused');

// ---- spots ----
const T0 = Date.UTC(2026, 9, 8, 14, 0, 0);
function msg(o) {
  return JSON.stringify(Object.assign({ sq: 1, f: 14075000, md: 'FT8', rp: -12, t: T0 / 1000 + 3, t_tx: T0 / 1000,
    sc: 'KB2SLO', sl: 'FN30', rc: 'K1ABC', rl: 'FN42ab', sa: 291, ra: 291, b: '20m' }, o));
}
same(P.spotOf(msg({}), 'KB2SLO'),
  { rx: 'K1ABC', grid: 'FN42ab', band: '20m', mode: 'FT8', snr: -12, hz: 14075000, t: T0 },
  'spot: fields, timed by t_tx');
same(P.spotOf(msg({ t_tx: undefined }), 'kb2slo').t, T0 + 3000, 'spot: t when there is no t_tx');
same(P.spotOf(msg({ sc: 'W1AW' }), 'KB2SLO'), null, 'spot: another sender never shows as hearing us');
same(P.spotOf(msg({ rc: '' }), 'KB2SLO'), null, 'spot: no receiver');
same(P.spotOf('not json', 'KB2SLO'), null, 'spot: not JSON');
same(P.spotOf(msg({ rp: undefined }), 'KB2SLO').snr, null, 'spot: no SNR');

// ---- state ----
function spot(rx, t, band) {
  return { rx: rx, grid: '', band: band || '20m', mode: 'FT8', snr: -10, hz: 0, t: t };
}
let s = P.newState('KB2SLO');
same(P.addSpots(s, [spot('A', T0), spot('B', T0 + 15000)]), 2, 'spots: added');
same(P.addSpots(s, [spot('A', T0), spot('A', T0, '40m')]), 1, 'spots: the same report once; another band is another');
same(s.spots.length, 3, 'spots: three kept');
check(s.spots[0].t >= s.spots[1].t && s.spots[1].t >= s.spots[2].t, 'spots: newest first');

const big = P.newState('KB2SLO');
const many = [];
for (let i = 0; i < P.MAX_SPOTS + 10; i++) many.push(spot('R' + i, T0 + i));
P.addSpots(big, many);
same(big.spots.length, P.MAX_SPOTS, 'spots: capped');
same(big.spots[0].rx, 'R' + (P.MAX_SPOTS + 9), 'spots: the cap keeps the newest');

same(P.restore({ v: 1, call: 'W1AW', spots: [1], listen: [], tx: [] }, 'KB2SLO').spots, [], 'restore: another call starts fresh');
same(P.restore(null, 'KB2SLO').call, 'KB2SLO', 'restore: nothing saved');
same(P.restore({ v: 1, call: 'KB2SLO', spots: 'x', listen: [], tx: [] }, 'KB2SLO').spots, [], 'restore: a bad shape starts fresh');
const kept = { v: 1, call: 'KB2SLO', spots: [spot('A', T0)], listen: [], tx: [] };
same(P.restore(kept, 'KB2SLO'), kept, 'restore: this call kept');

s = P.newState('KB2SLO');
P.noteTx(s, T0);
P.noteTx(s, T0 + 30000);
P.noteTx(s, T0 + 30000 + P.TX_MERGE_MS + 1);
same(s.tx, [[T0, T0 + 30000], [T0 + 30000 + P.TX_MERGE_MS + 1, T0 + 30000 + P.TX_MERGE_MS + 1]],
  'tx: close transmissions merge, a pause starts a new stretch');

s = P.newState('KB2SLO');
same(P.wanted(s, T0), false, 'wanted: never transmitted');
P.noteTx(s, T0);
same(P.wanted(s, T0 + P.LAG_MS - 1), true, 'wanted: inside the lag after a transmission');
same(P.wanted(s, T0 + P.LAG_MS), false, 'wanted: past it');

// ---- gaps ----
s = P.newState('KB2SLO');
P.noteTx(s, T0);                       // need T0 .. T0+15 min
same(P.gaps(s, T0 + 5 * MIN), [{ from: T0, to: T0 + 5 * MIN }], 'gaps: transmitting and never listening');
P.listenStart(s, T0 + 2000);
P.listenAlive(s, T0 + 3 * MIN);         // listening 0:02 .. 3:00, then the phone locked
P.listenStart(s, T0 + 10 * MIN);        // back at 10:00
P.listenAlive(s, T0 + 20 * MIN);
same(P.gaps(s, T0 + 20 * MIN), [{ from: T0 + 3 * MIN, to: T0 + 10 * MIN }],
  'gaps: the locked stretch only; 2 s before the first subscribe is too short to show');
same(P.gaps(s, T0 + 40 * MIN), [{ from: T0 + 3 * MIN, to: T0 + 10 * MIN }],
  'gaps: nothing counts once the lag after the last transmission is over');

s = P.newState('KB2SLO');
P.listenStart(s, T0);
P.listenAlive(s, T0 + 60 * MIN);
same(P.gaps(s, T0 + 60 * MIN), [], 'gaps: listening without transmitting is never a gap');

s = P.newState('KB2SLO');
P.noteTx(s, T0);
P.listenStart(s, T0);
P.listenAlive(s, T0 + 2 * MIN);
same(P.gaps(s, T0 + 4 * MIN), [{ from: T0 + 2 * MIN, to: T0 + 4 * MIN }], 'gaps: one still open now');
same(P.gapLabel(P.gaps(s, T0 + 4 * MIN)[0], T0 + 4 * MIN), 'since 14:02z', 'gap label: open');
same(P.gapLabel({ from: T0 + 3 * MIN, to: T0 + 10 * MIN }, T0 + 20 * MIN), '14:03–14:10z', 'gap label: closed');

s = P.newState('KB2SLO');
P.noteTx(s, T0);
P.noteTx(s, T0 + 60 * MIN);             // two stretches an hour apart, never listening
same(P.gaps(s, T0 + 61 * MIN),
  [{ from: T0 + 60 * MIN, to: T0 + 61 * MIN }, { from: T0, to: T0 + 15 * MIN }],
  'gaps: one per stretch, newest first');

// ---- receivers, rows ----
// spot2: a fuller spot, for the sorting and summary tests.
function spot2(rx, t, band, snr, grid) {
  return { rx: rx, grid: grid == null ? '' : grid, band: band || '20m', mode: 'FT8', snr: snr == null ? null : snr, hz: 0, t: t };
}
const KM = { FN42: 300, JO31: 6000, QF56: 16000, EM95: 1000 };
const distKm = (g) => (g.slice(0, 4) in KM ? KM[g.slice(0, 4)] : null);

s = P.newState('KB2SLO');
P.addSpots(s, [
  spot2('NEAR', T0 + 9 * MIN, '20m', -5, 'FN42'),
  spot2('NEAR', T0 + 8 * MIN, '20m', -9, 'FN42'),   // older report, same band: counted, not a row
  spot2('NEAR', T0 + 7 * MIN, '40m', -15, 'FN42'),  // another band: its own row
  spot2('FAR', T0 + 5 * MIN, '20m', -20, 'QF56'),
  spot2('LOUD', T0 + 3 * MIN, '15m', 10, 'JO31'),
  spot2('NOGRID', T0 + 6 * MIN, '20m', null, ''),
  spot2('LATE', T0 + 2 * MIN, '20m', -1, ''),       // newest report has no locator...
  spot2('LATE', T0 + 1 * MIN, '20m', -2, 'EM95')    // ...an older one does
]);
let rx = P.receivers(s, { sort: 'time', distKm });
same(rx.map((r) => r.rx + '/' + r.band), ['NEAR/20m', 'NEAR/40m', 'NOGRID/20m', 'FAR/20m', 'LOUD/15m', 'LATE/20m'],
  'receivers: one row per receiver and band, newest first');
same([rx[0].count, rx[0].snr, rx[0].t], [2, -5, T0 + 9 * MIN], 'receivers: the newest report shows, and how many');
same([rx[5].grid, rx[5].km], ['EM95', 1000], "receivers: an older report's locator fills a newer one's gap");
same(P.receivers(s, { sort: 'distance', distKm }).map((r) => r.rx + '/' + r.band),
  ['FAR/20m', 'LOUD/15m', 'LATE/20m', 'NEAR/20m', 'NEAR/40m', 'NOGRID/20m'], 'receivers: farthest first, no grid last');
same(P.receivers(s, { sort: 'snr', distKm }).map((r) => r.rx + '/' + r.band),
  ['LOUD/15m', 'LATE/20m', 'NEAR/20m', 'NEAR/40m', 'FAR/20m', 'NOGRID/20m'], 'receivers: loudest first, no SNR last');
same(P.receivers(s, { band: '20m', sort: 'time', distKm }).map((r) => r.rx),
  ['NEAR', 'NOGRID', 'FAR', 'LATE'], 'receivers: one band');
same(P.bandsIn(s), ['40m', '20m', '15m'], 'bands: longest wavelength first');
same(P.sortBands(['70cm', '6m', '160m', '2m', 'x']), ['160m', '6m', '2m', '70cm', 'x'], 'bands: cm after m, unknown last');

let f = P.hourFacts(s, T0 + 10 * MIN, distKm);
same([f.stations, f.bands, f.farthest], [5, ['40m', '20m', '15m'], { rx: 'FAR', grid: 'QF56', km: 16000 }],
  'hour: distinct stations, bands, the farthest');
f = P.hourFacts(s, T0 + 64 * MIN, distKm);
same([f.stations, f.bands, f.farthest && f.farthest.rx], [3, ['40m', '20m'], 'FAR'], 'hour: LOUD, LATE and 15m drop out past the hour');
same(P.hourFacts(P.newState('X'), T0, distKm), { stations: 0, bands: [], farthest: null }, 'hour: nothing heard');

same(P.heardBy(s, 'near'), [
  { band: '20m', snr: -5, t: T0 + 9 * MIN, count: 2, grid: 'FN42' },
  { band: '40m', snr: -15, t: T0 + 7 * MIN, count: 1, grid: 'FN42' }], 'heard by: a row per band, newest first');
same(P.heardBy(s, 'W1AW'), [], 'heard by: a station that never reported us');
same(P.heardBy(null, 'NEAR'), [], 'heard by: no call set');

s = P.newState('KB2SLO');
P.noteTx(s, T0);
P.listenStart(s, T0 + 5 * MIN);
P.listenAlive(s, T0 + 20 * MIN);
P.addSpots(s, [spot('A', T0 + 6 * MIN), spot('B', T0 + 4 * MIN), spot('C', T0 - 10 * MIN)]);
let v = P.rows(s, T0 + 20 * MIN, { sort: 'time' }, 10);
same(v.rows.map((x) => (x.gap ? 'gap' : x.rx.rx)), ['A', 'gap', 'B', 'C'], 'rows: by time, the gap sits where it ended');
same(v.more, 0, 'rows: all shown');
v = P.rows(s, T0 + 20 * MIN, { sort: 'time' }, 1);
same(v.rows.map((x) => (x.gap ? 'gap' : x.rx.rx)), ['A'], 'rows: a gap older than the last row shown waits with it');
same(v.more, 2, 'rows: how many did not fit');
v = P.rows(s, T0 + 20 * MIN, { sort: 'snr' }, 10);
check(!v.rows.some((x) => x.gap), 'rows: no gap rows unless sorted by time');

// ---- prune ----
s = P.newState('KB2SLO');
P.addSpots(s, [spot('OLD', T0), spot('NEW', T0 + P.KEEP_MS)]);
P.noteTx(s, T0);
P.listenStart(s, T0);
P.prune(s, T0 + P.KEEP_MS + 1);
same(s.spots.map((x) => x.rx), ['NEW'], 'prune: spots past a day go');
same([s.tx.length, s.listen.length], [0, 0], 'prune: ranges past a day go');

if (failed) process.exit(1);
console.log('test_pskr_core: ok');
