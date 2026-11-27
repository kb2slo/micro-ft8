#!/usr/bin/env node
// Download ADI and Download full ADI read every QSO of a day (docs/architecture/logging.md, Export), by record
// newest first, against a stand-in for the station that cuts a page short the way the real link does: the
// main board answers at most 32 rows a page, so its 48-frame Port A queue never drops one, and the sidekick's
// reply holds about 18 rows and says so when it holds fewer.
// Asking for 50 rows a page by day, the downloads stopped at 18 QSOs a day (2026-11-26); each page now carries
// on from the last row of the one before.
//
// Runs app.html's own readLogPages(), fetchAllEntries(), downloadDayAdif() and downloadAllAdif() under Node,
// as test_nfer_export.js does.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');
const bandsSrc = fs.readFileSync(path.join(__dirname, '..', 'app', 'bands.js'), 'utf8');

function extract(name) {
  for (const marker of ['async function ' + name + '(', 'function ' + name + '(']) {
    const start = src.indexOf(marker);
    if (start === -1) continue;
    let i = src.indexOf('{', start), depth = 0, str = null, lc = false, bc = false;
    for (; i < src.length; i++) {
      const c = src[i], p = src[i - 1];
      if (lc) { if (c === '\n') lc = false; continue; }
      if (bc) { if (c === '/' && p === '*') bc = false; continue; }
      if (str) { if (c === str && p !== '\\') str = null; continue; }
      if (c === '/' && src[i + 1] === '/') { lc = true; continue; }
      if (c === '/' && src[i + 1] === '*') { bc = true; continue; }
      if (c === '"' || c === "'") { str = c; continue; }
      if (c === '{') depth++;
      if (c === '}' && --depth === 0) return src.slice(start, i + 1);
    }
  }
  throw new Error('function not found in app.html: ' + name);
}
function extractVar(name) {
  const m = src.match(new RegExp('var ' + name + ' = [^;]*;'));
  assert(m, 'var not found in app.html: ' + name);
  return m[0];
}

// One station's log: a busy day (59 QSOs), a day of 25, one of 3, each record with its ID in the order
// logged, served by record newest first as LOG_PAGE serves it.
const DAYS = { '20261114': 59, '20261113': 25, '20261111': 3 };
function row(day, i) {
  const hh = String(Math.floor(i / 60)).padStart(2, '0'), mm = String(i % 60).padStart(2, '0');
  return { time: hh + ':' + mm, band: '20m', call: 'K' + (i % 10) + 'A' + day.slice(6, 8) + String.fromCharCode(65 + (i % 26)),
           rst_sent: -10, rst_rcvd: -12, grid: 'FN42', freq: '14.074', my_grid: 'EM10', comment: '', mode: 'FT8',
           station_callsign: 'N0CALL', my_pota_ref: '', my_sota_ref: '' };
}
const LOG = [];
for (const day of Object.keys(DAYS).sort()) for (let i = 0; i < DAYS[day]; i++) LOG.push(Object.assign(row(day, i), { id: LOG.length + 1, date: day }));
const keyOf = (q) => [Number(q.date), Number(q.time.replace(':', '')), q.id];
const older = (a, b) => a[0] !== b[0] ? a[0] < b[0] : a[1] !== b[1] ? a[1] < b[1] : a[2] < b[2];
const MAIN_BOARD_ROWS = 32;      // kLogPageRows, poe_p4/main/porta_host.cpp: the most a page holds
const SIDEKICK_ROWS = 18;        // what FILE_ENTRIES_JSON_MAX holds of these rows
function station(p) {
  const u = new URL('http://station' + p);
  assert.strictEqual(u.pathname, '/api/log/entries');
  const take = +u.searchParams.get('take'), date = u.searchParams.get('date'), before = u.searchParams.get('before');
  const limit = before ? before.split('.').map(Number) : null;
  const match = LOG.filter((q) => !date || q.date === date).sort((a, b) => (older(keyOf(a), keyOf(b)) ? 1 : -1));
  // The main board answers at most MAIN_BOARD_ROWS, so its Port A queue never drops a page's newest rows;
  // the sidekick's buffer then holds what it can, and says when it held less.
  const page = match.filter((q) => !limit || older(keyOf(q), limit)).slice(0, Math.min(take, MAIN_BOARD_ROWS));
  const cut = page.length > SIDEKICK_ROWS;
  return Object.assign({ ok: true, entries: page.slice(0, SIDEKICK_ROWS), total: match.length }, cut ? { cut: true } : {});
}

const downloads = [];
const ctx = {
  setTimeout,
  dayFullRowsCache: {},
  logFetchJson: async (p) => station(p),
  triggerAdifDownload: (name, text) => downloads.push({ name, text }),
  failMsg: (m) => { throw new Error('failMsg: ' + m); },
};
vm.createContext(ctx);
vm.runInContext(bandsSrc, ctx);
vm.runInContext([extractVar('LOG_ENTRY_FETCH')].concat(
  ['adifField', 'adifFieldIfSet', 'adifBandTag', 'buildAdifRecord', 'dayActivationRefs', 'groupRowsByReference',
   'exportFileBaseName', 'logCursorOf', 'readLogPages', 'dayFullRows', 'fetchAllEntries', 'downloadDayNfer',
   'downloadDayAdif', 'downloadAllAdif']
    .map(extract)).join('\n'), ctx);

function calls(text) { return (text.match(/<call:\d+>(\w+)/g) || []).map((c) => c.replace(/<call:\d+>/, '')); }

(async () => {
  // The stand-in cuts a page of 50 as the station does, or the test proves nothing.
  const big = station('/api/log/entries?take=50&date=20261114');
  assert.strictEqual(big.entries.length, SIDEKICK_ROWS, 'a page of 50 comes back as 18 rows');
  assert.ok(big.cut && big.entries[0].time === '00:58', 'saying it was cut, the newest kept');

  for (const day of Object.keys(DAYS)) {
    const rows = await ctx.fetchAllEntries(day);
    assert.strictEqual(rows.length, DAYS[day], day + ': every row read');
    rows.forEach((q, i) => assert.strictEqual(q.time, row(day, i).time, day + ': row ' + i + ' in the order logged'));
  }

  const link = { textContent: '' };
  downloads.length = 0;
  await ctx.downloadDayAdif('20261114', link);
  assert.strictEqual(downloads.length, 1, 'Download ADI makes one file');
  assert.strictEqual(downloads[0].name, '20261114', 'named for its day');
  assert.deepStrictEqual(calls(downloads[0].text), Array.from({ length: 59 }, (_, i) => row('20261114', i).call),
                         'Download ADI holds the busy day\'s 59 QSOs, in order');

  downloads.length = 0;
  const seen = [];
  const watch = { set textContent(t) { seen.push(t); }, get textContent() { return 'Download full ADI'; } };
  await ctx.downloadAllAdif(watch);
  const want = Object.keys(DAYS).sort().reverse().flatMap((d) => Array.from({ length: DAYS[d] }, (_, i) => row(d, i).call));
  assert.deepStrictEqual(calls(downloads[0].text), want, 'Download full ADI holds all 87 QSOs, newest day first, each day in order');
  assert.ok(seen.includes('Preparing… 87/87'), 'and counts them as it reads: ' + seen.slice(-2));

  // A page cut short, as a larger one is: the next page carries on from its last row, so nothing is lost.
  const saved = ctx.LOG_ENTRY_FETCH;
  vm.runInContext('LOG_ENTRY_FETCH = 50', ctx);
  const cut = await ctx.fetchAllEntries('20261114');
  vm.runInContext('LOG_ENTRY_FETCH = ' + saved, ctx);
  assert.deepStrictEqual(Array.from(cut, (q) => q.time), Array.from({ length: 59 }, (_, i) => row('20261114', i).time),
                         'pages of 50, each cut to 18, still read the day whole');

  console.log('PASS: log downloads read every QSO by record through a link that cuts large pages short');
})().catch((e) => { console.error('FAIL:', e.message); process.exit(1); });
