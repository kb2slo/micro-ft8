#!/usr/bin/env node
// qrz_sync_core.js (docs/architecture/webapp.md, QRZ sync): which of the station's QSOs QRZ holds, which
// go up, the days asked for, the call budget, and a whole run against a stand-in relay. The records are
// made up, in the shapes the station's log API and the relay gave on 2026-11-19.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const ctx = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'qrz_sync_core.js'), 'utf8'), ctx);
const Q = vm.runInContext('QrzSync', ctx);
const plain = (x) => JSON.parse(JSON.stringify(x));

// A row as /api/log/entries gives it, with its day file's date; a record as the relay gives it.
const row = (date, time, call, band, mode, station) =>
  ({ date, time, call, band: band || '20m', mode: mode || 'FT8', station_callsign: station || 'K1ABC', grid: 'FN42' });
const rec = (logid, date, time, call, band, mode) =>
  ({ app_qrzlog_logid: String(logid), call, qso_date: date, time_on: time, band: band || '20m', mode: mode || 'FT8',
    station_callsign: 'K1ABC', app_qrzlog_status: 'N' });

// --- minuteOf ---
assert.strictEqual(Q.minuteOf('20261119', '21:54'), Date.UTC(2026, 10, 19, 21, 54) / 60000);
assert.strictEqual(Q.minuteOf('20261119', '2154'), Q.minuteOf('20261119', '215459'), 'seconds are dropped, as QRZ drops them');
for (const [d, t] of [['19700101', '00:05'], ['19991231', '2359'], ['20261340', '1200'], ['20260230', '1200'],
  ['20261119', '2460'], ['', '1200'], ['20261119', ''], [undefined, undefined], ['2026-11-19', '1200']]) {
  assert.strictEqual(Q.minuteOf(d, t), null, 'not a usable date and time: ' + d + ' ' + t);
}

// --- match ---
{
  const ours = [
    row('20261115', '22:10', 'VA3ABC', '20m', 'FT4'),   // 0: at QRZ, to the minute, band in another case
    row('20261115', '22:13', 'KD6XYZ'),                 // 1: not at QRZ
    row('20261115', '23:58', 'W1AAA'),                  // 2: at QRZ as 00:03 the next day
    row('20261116', '10:00', 'W1AAA'),                  // 3: the same station again, ten minutes before QRZ's
    row('20261116', '10:40', 'W1AAA'),                  // 4: and again, 31 minutes from QRZ's nearest
    row('20261116', '11:00', 'N5XYZ', '40m'),           // 5: QRZ has it on 20 m only
    row('20261116', '11:05', 'N5XYZ', '20m', 'FT4'),    // 6: and in FT8 only
    row('20261116', '12:00', 'AA1A', '20m', 'FT8', 'K1ABC/P'),  // 7: logged under another call
    row('19700101', '00:03', 'ZZ9ZZ'),                  // 8: logged with the clock unset
  ];
  const theirs = [
    Object.assign(rec(101, '20261115', '2210', 'va3abc', '20M', 'MFSK'), { submode: 'FT4' }),  // as an upload of ours reads
    rec(102, '20261116', '0003', 'W1AAA'),
    rec(103, '20261116', '1010', 'W1AAA'),
    rec(104, '20261116', '1100', 'N5XYZ', '20m', 'FT8'),
    rec(105, '20261101', '0900', 'G4ABC', '15m', 'SSB'),   // made on another radio
  ];
  const m = plain(Q.match(ours, theirs, 'k1abc'));
  assert.deepStrictEqual(m.pairs, [{ ours: 0, theirs: 0 }, { ours: 2, theirs: 1 }, { ours: 3, theirs: 2 }],
    'the same minute, across midnight, and the nearer of two QSOs with one station');
  assert.deepStrictEqual(m.unsent, [1, 4, 5, 6], 'not at QRZ: no record, past the window, another band, another mode');
  assert.deepStrictEqual(m.skipped, [{ ours: 7, why: 'callsign' }, { ours: 8, why: 'date' }]);
  assert.deepStrictEqual(m.extra, [3, 4], 'QRZ\'s records with no QSO here');
}
{
  // Two QSOs with one station and two records: each record is used once, nearest first.
  const ours = [row('20261116', '10:00', 'W1AAA'), row('20261116', '10:20', 'W1AAA')];
  const m = plain(Q.match(ours, [rec(1, '20261116', '1019', 'W1AAA'), rec(2, '20261116', '1002', 'W1AAA')], 'K1ABC'));
  assert.deepStrictEqual(m.pairs, [{ ours: 0, theirs: 1 }, { ours: 1, theirs: 0 }]);
  // One record between two QSOs goes to the nearer, and the other QSO is unsent.
  // QRZ's own way of writing FT4, as a mode, matches too.
  assert.strictEqual(plain(Q.match([row('20261115', '22:10', 'VA3ABC', '20m', 'FT4')], [rec(1, '20261115', '2210', 'VA3ABC', '20m', 'FT4')], 'K1ABC')).pairs.length, 1);
  const one = plain(Q.match(ours, [rec(1, '20261116', '1012', 'W1AAA')], 'K1ABC'));
  assert.deepStrictEqual([one.pairs, one.unsent], [[{ ours: 1, theirs: 0 }], [0]]);
  // Exactly the window is in; a minute more is out.
  const edge = (t) => plain(Q.match([row('20261116', '10:00', 'W1AAA')], [rec(1, '20261116', t, 'W1AAA')], 'K1ABC')).pairs.length;
  assert.deepStrictEqual([edge('1030'), edge('1031'), edge('0930'), edge('0929')], [1, 0, 1, 0]);
  assert.strictEqual(Q.WINDOW_MIN, 30);
  // Nothing at QRZ: everything usable is unsent. Nothing here: nothing to do.
  assert.deepStrictEqual(plain(Q.match(ours, [], 'K1ABC')).unsent, [0, 1]);
  assert.deepStrictEqual(plain(Q.match([], [rec(1, '20261116', '1012', 'W1AAA')], 'K1ABC')), { pairs: [], unsent: [], skipped: [], extra: [0] });
}

// --- range and daysToRead ---
assert.deepStrictEqual(plain(Q.range([row('20261115', '23:58', 'A'), row('20261111', '00:01', 'B'), row('19700101', '00:00', 'C'),
  row('20261230', '12:00', 'D', '20m', 'FT8', 'OTHER')], 'K1ABC')), { from: '2026-11-10', to: '2026-11-16' },
'a day either side of the usable QSOs under this logbook\'s call');
assert.deepStrictEqual(plain(Q.range([row('20260101', '00:00', 'A'), row('20261231', '23:59', 'B')], 'K1ABC')),
  { from: '2025-12-31', to: '2027-01-01' });
assert.strictEqual(Q.range([row('19700101', '00:00', 'C')], 'K1ABC'), null);
assert.strictEqual(Q.range([], 'K1ABC'), null);
const FILES = ['20261119.adi', '20261111.adi', 'Station.txt', '20261115.adi', '2026111.adi', '20261113.txt'];
assert.deepStrictEqual(plain(Q.daysToRead(FILES, '')), ['20261111.adi', '20261115.adi', '20261119.adi']);
assert.deepStrictEqual(plain(Q.daysToRead(FILES, '20261115')), ['20261115.adi', '20261119.adi'], 'the last clean day is read again');
assert.deepStrictEqual(plain(Q.daysToRead(undefined, '')), []);

// --- the call budget ---
{
  const MIN = 60 * 1000;
  const t0 = 1000 * 60 * MIN;
  assert.deepStrictEqual(plain(Q.mayRun(undefined, t0, false)), { ok: true, why: '', waitMs: 0 }, 'a first run may go');
  let b = Q.noteRun(undefined, t0, 3, true);
  assert.deepStrictEqual(plain(Q.mayRun(b, t0 + 4 * MIN, false)), { ok: false, why: 'recent', waitMs: 1 * MIN }, 'on its own, five minutes after the last');
  assert.strictEqual(Q.mayRun(b, t0 + 5 * MIN, false).ok, true);
  assert.strictEqual(Q.mayRun(b, t0 + 1, true).ok, true, 'one the operator asks for does not wait for that');
  // After a failure every run waits, longer each time, up to an hour.
  b = Q.noteRun(b, t0 + 5 * MIN, 1, false);
  assert.deepStrictEqual(plain(Q.mayRun(b, t0 + 5 * MIN + 1, true)), { ok: false, why: 'failed', waitMs: 1 * MIN - 1 });
  assert.deepStrictEqual(plain(Q.mayRun(b, t0 + 6 * MIN, false)), { ok: false, why: 'recent', waitMs: 4 * MIN }, 'the longer wait is the one named');
  let at = t0 + 10 * MIN;
  const waits = [];
  for (let i = 0; i < 8; ++i) {
    b = Q.noteRun(b, at, 0, false);
    waits.push(Q.mayRun(b, at, true).waitMs / MIN);
  }
  assert.deepStrictEqual(waits, [2, 4, 8, 16, 32, 60, 60, 60]);
  b = Q.noteRun(b, at, 1, true);
  assert.strictEqual(Q.mayRun(b, at + 1, true).ok, true, 'a run that ends well clears it');
  // At most HOUR_CALLS relay calls an hour, asked for or not.
  b = Q.noteRun(undefined, t0, Q.HOUR_CALLS - 2, true);
  assert.strictEqual(Q.callsLeft(b, t0 + MIN), 2);
  b = Q.noteRun(b, t0 + 10 * MIN, 2, true);
  assert.strictEqual(Q.callsLeft(b, t0 + 20 * MIN), 0);
  assert.deepStrictEqual(plain(Q.mayRun(b, t0 + 20 * MIN, true)), { ok: false, why: 'hour', waitMs: 40 * MIN });
  assert.strictEqual(Q.mayRun(b, t0 + 60 * MIN, true).ok, true, 'the hour starts again');
  assert.strictEqual(Q.callsLeft(b, t0 + 60 * MIN), Q.HOUR_CALLS);
  assert.strictEqual(Q.noteRun(b, t0 + 61 * MIN, 4, true).hourCalls, 4);
  assert.strictEqual(Q.callsLeft(undefined, t0), Q.HOUR_CALLS);
}

// --- a whole run ---
function station(rows, held, opts) {
  opts = opts || {};
  const asked = [];
  const inserted = [];
  const deps = {
    key: 'ABCD-1234-EF56-7890',
    readRows: async () => rows,
    buildAdif: (q) => '<call:' + q.call.length + '>' + q.call + '<eor>',
    relay: async (p, body) => {
      asked.push([p, plain(body)]);
      assert.strictEqual(body.key, 'ABCD-1234-EF56-7890', 'every call carries the key');
      if (opts.down === p) throw new Error('offline');
      if (p === '/qrz/status') return opts.status || { ok: true, callsign: 'K1ABC', count: held.length };
      if (p === '/qrz/fetch') {
        const size = opts.page || 250;
        const left = held.filter((r) => Number(r.app_qrzlog_logid) >= body.after);
        const now = left.slice(0, size);
        return { ok: true, records: now, more: now.length >= size,
          next: now.length ? Number(now[now.length - 1].app_qrzlog_logid) + 1 : body.after };
      }
      if (p === '/qrz/insert') {
        if (opts.insert) return opts.insert(body.records);
        inserted.push(...body.records);
        return { ok: true, results: body.records.map((r, k) => ({ result: 'inserted', logid: String(900 + inserted.length + k) })) };
      }
      throw new Error('no such path ' + p);
    },
  };
  return { deps, asked, inserted };
}

(async function () {
  const rows = [row('20261115', '22:10', 'VA3ABC'), row('20261115', '22:13', 'KD6XYZ'), row('20261116', '10:00', 'W1AAA'),
    row('19700101', '00:03', 'ZZ9ZZ')];
  const held = [rec(101, '20261115', '2210', 'VA3ABC'), rec(102, '20261101', '0900', 'G4ABC')];

  // Looking only: the plan, and nothing sent.
  let s = station(rows, held);
  let r = plain(await Q.run(Object.assign(s.deps, { send: false })));
  assert.deepStrictEqual([r.ok, r.logbook, r.rows, r.held, r.extra, r.calls], [true, 'K1ABC', 4, 1, 1, 2]);
  assert.deepStrictEqual(r.plan.map((q) => q.call), ['KD6XYZ', 'W1AAA']);
  assert.deepStrictEqual(r.skipped.map((x) => [x.row.call, x.why]), [['ZZ9ZZ', 'date']]);
  assert.deepStrictEqual([r.sent, r.unsent, r.cleanThrough, s.inserted.length], [0, 2, '', 0],
    'a look sends nothing; the oldest unsent QSO is on the first day, so no day is settled');
  assert.deepStrictEqual(s.asked.map((a) => a[0]), ['/qrz/status', '/qrz/fetch']);
  assert.deepStrictEqual(s.asked[1][1], { from: '2026-11-14', to: '2026-11-17', after: 0, key: 'ABCD-1234-EF56-7890' },
    'with no version given, none is sent');

  // The run itself: what QRZ lacks goes up, in one batch. The app's version rides on every call.
  s = station(rows, held);
  r = plain(await Q.run(Object.assign(s.deps, { version: '0.2.0' })));
  assert.deepStrictEqual(s.asked.map((a) => a[1].app), ['0.2.0', '0.2.0', '0.2.0']);
  assert.deepStrictEqual([r.ok, r.sent, r.duplicates, r.refused, r.unsent, r.calls, r.cleanThrough], [true, 2, 0, [], 0, 3, '20261116']);
  assert.deepStrictEqual(s.inserted, ['<call:6>KD6XYZ<eor>', '<call:5>W1AAA<eor>']);

  // Run again, with QRZ now holding them: nothing to send, and no insert call.
  const after = held.concat([rec(103, '20261115', '2213', 'KD6XYZ'), rec(104, '20261116', '1000', 'W1AAA')]);
  s = station(rows, after);
  r = plain(await Q.run(s.deps));
  assert.deepStrictEqual([r.ok, r.held, r.plan.length, r.sent, r.calls, r.cleanThrough], [true, 3, 0, 0, 2, '20261116']);

  // QRZ's pages are followed to the end.
  s = station(rows, after, { page: 2 });
  r = plain(await Q.run(s.deps));
  assert.deepStrictEqual([r.held, r.plan.length, r.calls], [3, 0, 4], 'status, then pages of 2, 2 and 0');
  assert.deepStrictEqual(s.asked.slice(1).map((a) => a[1].after), [0, 103, 105]);

  // More to send than a batch holds.
  const many = Array.from({ length: 120 }, (_, i) => row('20261117', '10:00', 'W' + (1000 + i)));
  s = station(many, []);
  r = plain(await Q.run(s.deps));
  assert.deepStrictEqual([r.sent, r.calls, s.inserted.length], [120, 14, 120], 'status, one page, twelve batches of at most 10');
  assert.deepStrictEqual(s.asked.slice(2).map((a) => a[1].records.length), Array(12).fill(10));

  // A duplicate is not a failure; a refusal is reported with its row, and the day is not settled.
  s = station(rows, held, { insert: () => ({ ok: true, results: [{ result: 'duplicate' }, { result: 'refused', reason: 'bad band' }] }) });
  r = plain(await Q.run(s.deps));
  assert.deepStrictEqual([r.ok, r.sent, r.duplicates, r.unsent, r.cleanThrough], [true, 0, 1, 1, '20261115'],
    'the refused QSO is still unsent, and the days before its own are settled');
  assert.deepStrictEqual(r.refused.map((x) => [x.row.call, x.reason]), [['W1AAA', 'bad band']]);

  // Sending as QSOs are logged: only those from a day on go up, and older ones wait for the operator.
  const mixed = [row('20261114', '10:00', 'OLD1'), row('20261114', '10:05', 'OLD2'), row('20261118', '09:00', 'NEW1'),
    row('20261119', '09:00', 'NEW2')];
  s = station(mixed, []);
  r = plain(await Q.run(Object.assign(s.deps, { sendFrom: '20261118' })));
  assert.deepStrictEqual(s.inserted, ['<call:4>NEW1<eor>', '<call:4>NEW2<eor>']);
  assert.deepStrictEqual([r.ok, r.plan.length, r.sent, r.unsent, r.cleanThrough], [true, 4, 2, 2, ''],
    'the two older ones are planned, not sent, and hold every day back');
  s = station(mixed, [rec(1, '20261114', '1000', 'OLD1'), rec(2, '20261114', '1005', 'OLD2')]);
  r = plain(await Q.run(Object.assign(s.deps, { sendFrom: '20261119' })));
  assert.deepStrictEqual([s.inserted, r.unsent, r.cleanThrough], [['<call:4>NEW2<eor>'], 1, '20261114'],
    'with the older days at QRZ, they are settled up to the day before the one still waiting');
  s = station(mixed, []);
  r = plain(await Q.run(Object.assign(s.deps, { sendFrom: '20261120' })));
  assert.deepStrictEqual([r.ok, r.sent, r.unsent, r.calls, s.inserted.length], [true, 0, 4, 2, 0], 'nothing new: no insert call');
  s = station(mixed, []);
  r = plain(await Q.run(Object.assign(s.deps, { sendFrom: '20261118', send: false })));
  assert.deepStrictEqual([r.sent, r.unsent, s.inserted.length], [0, 4, 0], 'a look is a look, whatever day is named');

  // A key QRZ refuses, at the start and part way through a batch.
  s = station(rows, held, { status: { ok: false, error: 'auth', reason: 'invalid api key' } });
  r = plain(await Q.run(s.deps));
  assert.deepStrictEqual([r.ok, r.error, r.reason, r.calls, s.asked.length], [false, 'auth', 'invalid api key', 1, 1]);
  s = station(rows, held, { insert: () => ({ ok: false, error: 'auth', reason: 'invalid api key', results: [{ result: 'inserted', logid: '7' }] }) });
  r = plain(await Q.run(s.deps));
  assert.deepStrictEqual([r.ok, r.error, r.sent, r.cleanThrough], [false, 'auth', 1, ''], 'what went up before the refusal is counted');

  // The relay out of reach, at each step.
  for (const down of ['/qrz/status', '/qrz/fetch', '/qrz/insert']) {
    s = station(rows, held, { down });
    r = plain(await Q.run(s.deps));
    assert.deepStrictEqual([r.ok, r.error, r.cleanThrough], [false, 'unreachable', ''], down);
  }

  // A logbook already known is not asked for again; a station that does not answer is said so.
  s = station(rows, held);
  r = plain(await Q.run(Object.assign(s.deps, { logbook: 'k1abc', send: false })));
  assert.deepStrictEqual([r.ok, r.logbook, r.held, r.calls, s.asked.map((a) => a[0])], [true, 'K1ABC', 1, 1, ['/qrz/fetch']]);
  s = station(rows, held);
  s.deps.readRows = async () => { throw new Error('timeout'); };
  r = plain(await Q.run(s.deps));
  assert.deepStrictEqual([r.ok, r.error, r.calls], [false, 'station', 1]);

  // The run stops at its call allowance instead of going over it.
  s = station(many, []);
  r = plain(await Q.run(Object.assign(s.deps, { maxCalls: 3 })));
  assert.deepStrictEqual([r.ok, r.error, r.calls, r.sent, s.asked.length], [false, 'budget', 3, 10, 3]);
  s = station(rows, held);
  r = plain(await Q.run(Object.assign(s.deps, { maxCalls: 0 })));
  assert.deepStrictEqual([r.ok, r.error, s.asked.length], [false, 'budget', 0]);

  // A logbook of another callsign than the station logs under: every row is skipped, and nothing is asked
  // of QRZ beyond its status.
  s = station(rows, held, { status: { ok: true, callsign: 'W9OTHER' } });
  r = plain(await Q.run(s.deps));
  assert.deepStrictEqual([r.ok, r.plan.length, r.calls, r.skipped.filter((x) => x.why === 'callsign').length], [true, 0, 1, 4]);

  // QRZ never running out of pages is given up on.
  s = station(rows, held);
  s.deps.relay = async (p, body) => (p === '/qrz/status' ? { ok: true, callsign: 'K1ABC' }
    : { ok: true, records: [], more: true, next: body.after + 1 });
  r = plain(await Q.run(Object.assign(s.deps, { maxCalls: 1000 })));
  assert.deepStrictEqual([r.ok, r.error, r.calls], [false, 'too_many', 1 + Q.MAX_PAGES]);

  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
