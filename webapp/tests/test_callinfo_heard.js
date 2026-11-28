#!/usr/bin/env node
// callinfo.js's heard section (row c5r3; docs/architecture/webapp.md, the callsign card): the label with where
// the station's history starts, a line per band and mode, a decode's row, and the hourly trend's line.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const stub = () => new Proxy(function () {}, { get: () => stub(), apply: () => stub(), set: () => true });
const ctx = vm.createContext({ setInterval() {}, fetch: () => Promise.reject(new Error('offline')), window: {}, document: stub() });
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'callinfo.js'), 'utf8'), ctx);
const C = ctx.window.CallInfo;

// 2026-11-27 14:02:00z and 16:41:15z.
const t1402 = Date.UTC(2026, 10, 27, 14, 2, 0) / 1000;
const t1641 = Date.UTC(2026, 10, 27, 16, 41, 15) / 1000;

// A line per band and mode.
const g = { band: '20m', sl: 150, n: 40, first: t1402, last: t1641, now: -7, median: -12, best: -3, hourly: [] };
assert.strictEqual(C.heardGroupText(g), '20m FT8 ×40 · 14:02–16:41z · -7 dB now, median -12, best -3');
assert.strictEqual(C.heardGroupText(Object.assign({}, g, { sl: 75, n: 1, last: t1402, now: 4, median: 4, best: 4 })),
  '20m FT4 ×1 · 14:02z · +4 dB now, median +4, best +4', 'heard once: one time, FT4 by its slot, + on a positive SNR');
assert.strictEqual(C.heardGroupText(Object.assign({}, g, { last: t1402 + 45 })),
  '20m FT8 ×40 · 14:02z · -7 dB now, median -12, best -3', 'heard within one minute: one time, not 14:02–14:02z');
assert.strictEqual(C.heardGroupText(Object.assign({}, g, { first: t1402 - 86400 + 8 * 3600 })),
  '20m FT8 ×40 · Nov 26 22:02z – Nov 27 16:41z · -7 dB now, median -12, best -3', 'across UTC midnight: each end dated');
assert.strictEqual(C.heardGroupText(Object.assign({}, g, { first: 0, last: 0 })),
  '20m FT8 ×40 · -7 dB now, median -12, best -3', 'heard with no clock: no times');
assert.strictEqual(C.heardGroupText(Object.assign({}, g, { band: '' })),
  'FT8 ×40 · 14:02–16:41z · -7 dB now, median -12, best -3', 'a dial on no band: the mode alone');

// A decode.
const slot = Math.floor(t1641 / 15);
assert.strictEqual(C.heardRowText({ s: 9, x: 'CQ K1ABC FN42', snr: -7, hz: 1500, si: slot, sl: 150, fq: 14074000 }),
  '16:41:15z · 20m FT8 · -7 dB · 1500 Hz · CQ K1ABC FN42');
assert.strictEqual(C.heardRowText({ s: 9, x: 'K1ABC W9XYZ RR73', snr: 2, hz: 900, si: 2 * slot + 1, sl: 75, fq: 7047500 }),
  '16:41:22z · 40m FT4 · +2 dB · 900 Hz · K1ABC W9XYZ RR73', 'FT4 timed by its 7.5 s slots');
assert.strictEqual(C.heardRowText({ s: 9, x: 'CQ K1ABC FN42', snr: -7, hz: 1500, si: 0, sl: 150, fq: 0 }),
  'FT8 · -7 dB · 1500 Hz · CQ K1ABC FN42', 'no clock and no dial: neither shown');

// The label.
const now = Date.UTC(2026, 10, 27, 17, 0, 0);
assert.strictEqual(C.heardLabelText({ total: 46, since: t1402 }, now), 'Heard (46) · since 14:02z');
assert.strictEqual(C.heardLabelText({ total: 46, since: t1402 - 86400 }, now), 'Heard (46) · since Nov 26 14:02z',
  'a history from before today is dated');
assert.strictEqual(C.heardLabelText({ total: 0, since: 0 }, now), 'Heard (0)', 'nothing timed: no since');

// The trend.
const svg = C.heardSparkSvg([null, null, -18, -14, null, -9, -7, null, null, -5]);
assert.match(svg, /^<svg class=ci-spark /);
assert.strictEqual((svg.match(/<path /g) || []).length, 2, 'two runs of hours, broken at the gap');
assert.strictEqual((svg.match(/<circle /g) || []).length, 1, 'an hour alone is a dot');
assert.match(svg, /aria-label="Hourly median SNR, oldest first: none, none, -18, -14, none, -9, -7, none, none, -5 dB"/);
const ys = (svg.match(/<path d="([^"]+)"/)[1]).split(/ L|M/).filter(Boolean).map((p) => Number(p.split(' ')[1]));
assert.ok(ys[0] > ys[1], 'stronger is higher: -14 is drawn above -18');
assert.strictEqual(C.heardSparkSvg([null, null]), '', 'nothing heard: no chart');
assert.strictEqual(C.heardSparkSvg([null, -9, null]), '', 'one hour heard is no trend: no chart');
assert.strictEqual(C.heardSparkSvg(undefined), '');
const flat = C.heardSparkSvg([-10, -10, -10]);
const flatYs = (flat.match(/<path d="([^"]+)"/)[1]).split(/ L|M/).filter(Boolean).map((p) => Number(p.split(' ')[1]));
assert.ok(flatYs.every((y) => y === flatYs[0]) && flatYs[0] > 2 && flatYs[0] < 18, 'a steady station is a level line mid-chart');

console.log('test_callinfo_heard: ok');
