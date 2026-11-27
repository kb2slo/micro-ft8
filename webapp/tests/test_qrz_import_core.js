#!/usr/bin/env node
// qrz_import_core.js (docs/architecture/webapp.md, QRZ sync): which of a QRZ logbook's records come in, the
// fields kept, the slices a record is cut into for the main board, the requests they go in, and a whole import
// against a stand-in relay and station. The records are made up, in the shape the relay gives.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const ctx = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'qrz_import_core.js'), 'utf8'), ctx);
const Q = vm.runInContext('QrzImport', ctx);
const plain = (x) => JSON.parse(JSON.stringify(x));

const rec = (id, call, mode, extra) => Object.assign({ app_qrzlog_logid: String(id), call, qso_date: '20240301', time_on: '2210',
  band: '20m', mode, freq: '14.074', station_callsign: 'KB2SLO', gridsquare: 'FN03', rst_sent: '-05', rst_rcvd: '-11',
  app_qrzlog_status: 'C', dxcc: '1', cont: 'NA', state: 'ON', cnty: 'Toronto', cqz: '5', ituz: '4' }, extra || {});

// Only FT8 and FT4, FT4 as QRZ writes it either way.
assert.strictEqual(Q.modeOf({ mode: 'ft8' }), 'FT8');
assert.strictEqual(Q.modeOf({ mode: 'FT4' }), 'FT4');
assert.strictEqual(Q.modeOf({ mode: 'MFSK', submode: 'FT4' }), 'FT4');
for (const m of [{ mode: 'SSB' }, { mode: 'CW' }, { mode: 'MFSK', submode: 'JS8' }, { mode: 'JT65' }, {}]) {
  assert.strictEqual(Q.modeOf(m), '', 'not ours: ' + JSON.stringify(m));
}
assert.strictEqual(Q.leftOut(rec(1, 'K1ABC', 'FT8')), '');
assert.strictEqual(Q.leftOut(rec(1, 'K1ABC', 'SSB')), 'mode');
assert.strictEqual(Q.leftOut(rec(1, 'K1ABC', 'FT8', { qso_date: '' })), 'date', 'no date: left out');
assert.strictEqual(Q.leftOut(rec(0, 'K1ABC', 'FT8')), 'call', 'no QRZ ID: cannot be found again');

// The fields kept, FT4 as the station logs its own; a value cannot break the ADIF around it.
const f = Q.fieldsOf(rec(7, 'va3abc', 'FT4', { name: 'Somebody', email: 'x@y', address: '1 Main St', comment: 'tnx' }));
assert.deepStrictEqual(plain(f.slice(0, 7)), ['<call:6>VA3ABC', '<qso_date:8>20240301', '<time_on:4>2210', '<band:3>20m',
  '<mode:4>MFSK', '<submode:3>FT4', '<freq:6>14.074']);
assert.ok(!f.some((x) => /name|email|address|comment/.test(x)), 'never a name, address or email: ' + f.join(''));
assert.ok(Q.fieldsOf(rec(7, 'K1ABC', 'FT8', { cnty: 'Bad<eor>\nCounty' })).includes('<cnty:14>Bad eor County'),
  'a value with ADIF in it is made plain');
assert.deepStrictEqual(plain(Q.fieldsOf(rec(7, 'K1ABC', 'FT8')).filter((x) => /mode/.test(x))), ['<mode:3>FT8'], 'FT8 has no submode');

// Slices: each names QRZ's ID and fits a frame, the call in the first, every field in one of them.
const long = rec(1531992047, 'VA3ABC', 'FT4', { cnty: 'X'.repeat(60), state: 'Y'.repeat(40), app_qrzlog_qsldate: '20240310' });
const sl = Q.slices(long);
assert.ok(sl.length >= 2, 'a long record takes more than one slice');
sl.forEach((s) => {
  assert.ok(s.length <= Q.SLICE_MAX, 'fits a frame: ' + s.length);
  assert.ok(s.startsWith('<app_qrzlog_logid:10>1531992047'), 'names QRZ\'s ID');
});
assert.ok(sl[0].includes('<call:6>VA3ABC'), 'the call is in the first');
const all = sl.map((s) => s.replace('<app_qrzlog_logid:10>1531992047', '')).join('');
assert.strictEqual(all, Q.fieldsOf(long).join(''), 'every field once, in order');
assert.deepStrictEqual(plain(Q.slices(rec(9, 'K1ABC', 'FT8', { cnty: 'Z'.repeat(300) })).filter((s) => /cnty/.test(s))), [],
  'a field too long for any slice is left out, the rest kept');

// Requests: within the sidekick's buffer, a record's slices together.
const recs = Array.from({ length: 30 }, (_, i) => ({ call: 'K' + i, lines: Q.slices(rec(100 + i, 'K' + i + 'ABC', 'FT8')) }));
const b = Q.batches(recs);
b.forEach((batch) => {
  const bytes = batch.reduce((n, r) => n + r.lines.reduce((m, l) => m + l.length + 1, 0), 0);
  assert.ok(bytes <= Q.BODY_MAX, 'a request fits the sidekick: ' + bytes);
});
assert.strictEqual(b.reduce((n, x) => n + x.length, 0), 30, 'every record in one request');

// A whole import.
(async function () {
  const book = [rec(1, 'K1ABC', 'FT8'), rec(2, 'W9XY', 'MFSK', { submode: 'FT4' }), rec(3, 'G4ABC', 'SSB'),
    rec(4, 'DL1AA', 'FT8', { qso_date: '' }), rec(5, 'VE3XYZ', 'FT4'), rec(6, 'BAD1', 'FT8')];
  const asked = [], posted = [];
  const deps = {
    key: 'ABCD-1234', version: '0.4.0', today: '2026-11-27',
    relay: async (p, body) => {
      asked.push([p, plain(body)]);
      if (p !== '/qrz/fetch') throw new Error('no such path ' + p);
      const page = book.filter((r) => Number(r.app_qrzlog_logid) >= body.after).slice(0, 4);
      return { ok: true, records: page, more: page.length === 4, next: page.length ? Number(page[page.length - 1].app_qrzlog_logid) + 1 : body.after };
    },
    post: async (lines) => {
      posted.push(...lines);
      const refused = [];
      lines.forEach((l, i) => { if (/BAD1/.test(l)) refused.push({ line: i, why: 'bad fields' }); });
      return { taken: lines.length - refused.length, refused };
    },
  };
  const seen = [];
  deps.progress = (done, total) => seen.push([done, total]);
  const r = plain(await Q.run(deps));
  assert.deepStrictEqual([r.ok, r.fetched, r.imported, r.leftOut, r.refused, r.calls],
    [true, 6, 3, { mode: 1, date: 1, call: 0 }, [{ call: 'BAD1', why: 'bad fields' }], 2]);
  assert.deepStrictEqual(asked[0][1], { from: '1980-01-01', to: '2026-11-28', after: 0, key: 'ABCD-1234', app: '0.4.0' },
    'the whole logbook, to a day past today');
  assert.ok(!posted.some((l) => /G4ABC|DL1AA/.test(l)), 'nothing left out reaches the station');
  assert.deepStrictEqual(seen[seen.length - 1], [4, 4], 'and progress counts to the end');

  // Out of relay calls, the relay out of reach, the station not answering.
  let x = plain(await Q.run(Object.assign({}, deps, { maxCalls: 1 })));
  assert.deepStrictEqual([x.ok, x.error, x.imported], [false, 'budget', 0]);
  x = plain(await Q.run(Object.assign({}, deps, { relay: async () => { throw new Error('offline'); } })));
  assert.strictEqual(x.error, 'unreachable');
  x = plain(await Q.run(Object.assign({}, deps, { relay: async () => ({ ok: false, error: 'auth', reason: 'invalid api key' }) })));
  assert.deepStrictEqual([x.error, x.reason], ['auth', 'invalid api key']);
  x = plain(await Q.run(Object.assign({}, deps, { post: async () => { throw new Error('timeout'); } })));
  assert.deepStrictEqual([x.ok, x.error], [false, 'station']);
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
