#!/usr/bin/env node
// callinfo.js's lines from the main board's log summary (docs/architecture/webapp.md, QSOs and the callsign
// card): the QSOs tab's "303 QSOs · Nov 11 – Nov 27, 2026 · 16 days on the air", the card's
// "20m FT8 ×3 · 40m FT4 ×1 · first …, last …", and one QSO of the card's list.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

// callinfo.js builds its card DOM on load; any element is a stub that accepts every call.
const stub = () => new Proxy(function () {}, { get: () => stub(), apply: () => stub(), set: () => true });
const ctx = vm.createContext({ setInterval() {}, fetch: () => Promise.reject(new Error('offline')), window: {}, document: stub() });
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'callinfo.js'), 'utf8'), ctx);
const C = ctx.window.CallInfo;

// The QSOs tab.
assert.strictEqual(C.logSummaryText(303, { first: '20261111', last: '20261127', days: 16 }),
  '303 QSOs · Nov 11 – Nov 27, 2026 · 16 days on the air');
assert.strictEqual(C.logSummaryText(1, { first: '20261127', last: '20261127', days: 1 }), '1 QSO · Nov 27, 2026 · 1 day on the air',
  'one QSO on one day: the day alone');
assert.strictEqual(C.logSummaryText(40, { first: '20251230', last: '20260102', days: 3 }),
  '40 QSOs · Dec 30, 2025 – Jan 2, 2026 · 3 days on the air', 'across a new year, both years');
assert.strictEqual(C.logSummaryText(0, { first: '', last: '', days: 0 }), '', 'no QSO: no line');
assert.strictEqual(C.logSummaryText(5, null), '5 QSOs', 'a station that sends no summary still gives the count');
assert.strictEqual(C.logSummaryText(1540, { first: '20260528', last: '20261127', days: 119 }),
  '1,540 QSOs · May 28 – Nov 27, 2026 · 119 days on the air', 'thousands with a comma, as the import\'s line counts them');
assert.strictEqual(C.logSummaryText(5, { first: '', last: '', days: 0 }), '5 QSOs', 'nor dates when none has one');

// The card.
const pairs = [{ band: '20M', mode: 'FT8', n: 3 }, { band: '40M', mode: 'FT4', n: 1 }];
assert.strictEqual(C.callSummaryText({ first: '20261111', last: '20261125', days: 3, pairs_total: 2, pairs }),
  '20m FT8 ×3 · 40m FT4 ×1 · first Nov 11, 2026, last Nov 25, 2026');
assert.strictEqual(C.callSummaryText({ first: '20261125', last: '20261125', days: 1, pairs_total: 1, pairs: pairs.slice(0, 1) }),
  '20m FT8 ×3 · Nov 25, 2026', 'worked on one day: that day');
assert.strictEqual(C.callSummaryText({ first: '20261111', last: '20261125', days: 3, pairs_total: 14, pairs }),
  '20m FT8 ×3 · 40m FT4 ×1 · and 12 more · first Nov 11, 2026, last Nov 25, 2026', 'bands and modes past what the summary names');
assert.strictEqual(C.callSummaryText(null), '');
assert.strictEqual(C.callSummaryText({ first: '20261111', last: '20261127', days: 2, pairs_total: 1, pairs: pairs.slice(0, 1) }, '20261127'),
  '20m FT8 ×3 · first Nov 11, 2026, last Nov 27, 2026 · worked today', 'last worked today: says so');
assert.ok(!/today/.test(C.callSummaryText({ first: '20261111', last: '20261126', days: 2, pairs_total: 1, pairs: pairs.slice(0, 1) }, '20261127')),
  'not when the last was yesterday');

assert.strictEqual(C.callQsoText({ date: '20261125', time: '21:54', band: '20m', mode: 'FT8', freq: '14.074', rst_sent: -8, rst_rcvd: 3 }),
  '2026-11-25 21:54 · 20m FT8 · 14.074 MHz · -8/+3');
assert.strictEqual(C.callQsoText({ date: '', time: '??:??', band: '', mode: '', freq: '', rst_sent: -99, rst_rcvd: -99 }),
  '??:?? · —/—', 'a record with little in it still reads');

console.log('test_callinfo_log: ok');
