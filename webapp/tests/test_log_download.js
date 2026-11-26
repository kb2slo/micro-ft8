#!/usr/bin/env node
// Download ADI and Download full ADI read every QSO of a day (docs/architecture/logging.md, Export), against a
// stand-in for the station that cuts a page short the way the real link does: the main board's Port A queue
// holds 48 frames and drops the oldest, and the sidekick's reply holds about 18 rows (row ujtv). Asking for 50
// rows a page, the downloads stopped at 18 QSOs a day and lost a day's first rows (2026-11-26).
//
// Runs app.html's own fetchAllEntries(), downloadDayAdif() and downloadAllAdif() under Node, as
// test_nfer_export.js does.
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

// One station's log: a busy day (59 QSOs), a day of 25, one of 3.
const DAYS = { '20261114.adi': 59, '20261113.adi': 25, '20261111.adi': 3 };
function row(day, i) {
  const hh = String(Math.floor(i / 60)).padStart(2, '0'), mm = String(i % 60).padStart(2, '0');
  return { time: hh + ':' + mm, band: '20m', call: 'K' + (i % 10) + 'A' + day.slice(6, 8) + String.fromCharCode(65 + (i % 26)),
           rst_sent: -10, rst_rcvd: -12, grid: 'FN42', freq: '14.074', my_grid: 'EM10', comment: '', mode: 'FT8',
           station_callsign: 'N0CALL', my_pota_ref: '', my_sota_ref: '' };
}
const PORTA_OUT_QUEUE_LEN = 48;  // components/porta/include/porta_out_queue.h
const SIDEKICK_ROWS = 18;        // what FILE_ENTRIES_JSON_MAX holds of these rows
function station(p) {
  const u = new URL('http://station' + p);
  if (u.pathname === '/api/log/files') {
    const names = Object.keys(DAYS).sort().reverse();
    const skip = +u.searchParams.get('skip'), take = +u.searchParams.get('take');
    return { ok: true, files: names.slice(skip, skip + take) };
  }
  const day = u.searchParams.get('file'), skip = +u.searchParams.get('skip'), take = +u.searchParams.get('take');
  let page = [];
  for (let i = skip; i < Math.min(DAYS[day], skip + take); i++) page.push(row(day, i));
  const frames = page.length + 1;  // the rows, then the ACK
  if (frames > PORTA_OUT_QUEUE_LEN) page = page.slice(frames - PORTA_OUT_QUEUE_LEN);
  return { ok: true, entries: page.slice(0, SIDEKICK_ROWS) };
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
   'exportFileBaseName', 'dayFullRows', 'fetchAllEntries', 'downloadDayNfer', 'downloadDayAdif', 'downloadAllAdif']
    .map(extract)).join('\n'), ctx);

function calls(text) { return (text.match(/<call:\d+>(\w+)/g) || []).map((c) => c.replace(/<call:\d+>/, '')); }

(async () => {
  // The stand-in cuts a page of 50 as the station did, or the test proves nothing.
  const big = station('/api/log/entries?file=20261114.adi&skip=0&take=50').entries;
  assert.strictEqual(big.length, SIDEKICK_ROWS, 'a page of 50 comes back as 18 rows');
  assert.strictEqual(big[0].time, '00:03', 'and without the day\'s first rows');

  for (const day of Object.keys(DAYS)) {
    const rows = await ctx.fetchAllEntries(day);
    assert.strictEqual(rows.length, DAYS[day], day + ': every row read');
    rows.forEach((q, i) => assert.strictEqual(q.time, row(day, i).time, day + ': row ' + i + ' in order'));
  }

  const link = { textContent: '' };
  downloads.length = 0;
  await ctx.downloadDayAdif('20261114.adi', link);
  assert.strictEqual(downloads.length, 1, 'Download ADI makes one file');
  assert.deepStrictEqual(calls(downloads[0].text), Array.from({ length: 59 }, (_, i) => row('20261114.adi', i).call),
                         'Download ADI holds the busy day\'s 59 QSOs, in order');

  downloads.length = 0;
  await ctx.downloadAllAdif(link);
  const want = Object.keys(DAYS).sort().reverse().flatMap((d) => Array.from({ length: DAYS[d] }, (_, i) => row(d, i).call));
  assert.deepStrictEqual(calls(downloads[0].text), want, 'Download full ADI holds all 87 QSOs, newest day first');

  console.log('PASS: log downloads read every QSO through a link that cuts large pages short');
})().catch((e) => { console.error('FAIL:', e.message); process.exit(1); });
