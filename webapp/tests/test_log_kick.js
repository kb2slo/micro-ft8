#!/usr/bin/env node
// A logged QSO reaches the QSOs list at once (docs/architecture/webapp.md, Screens: QSOs): runs the real
// functions from app.html, as test_decode_mode.js does. Which of our transmissions log a QSO; a refresh
// asked for while one runs is run again after it, not dropped; and the wiring that starts one when such
// a transmission begins or the QSOs tab opens.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');

function extractFunction(text, name) {
  const start = text.indexOf('function ' + name + '(');
  assert(start !== -1, 'function not found in app.html: ' + name);
  let depth = 0, i = text.indexOf('{', start);
  let inStr = null, inLine = false, inBlock = false;
  for (; i < text.length; i++) {
    const c = text[i], prev = text[i - 1];
    if (inLine) { if (c === '\n') inLine = false; continue; }
    if (inBlock) { if (c === '/' && prev === '*') inBlock = false; continue; }
    if (inStr) { if (c === inStr && prev !== '\\') inStr = null; continue; }
    if (c === '/' && text[i + 1] === '/') { inLine = true; continue; }
    if (c === '/' && text[i + 1] === '*') { inBlock = true; continue; }
    if (c === '"' || c === "'") { inStr = c; continue; }
    if (c === '{') depth++;
    if (c === '}' && --depth === 0) { i++; break; }
  }
  const async = text.slice(start - 6, start) === 'async ' ? 'async ' : '';
  return async + text.slice(start, i);
}

// Which transmissions log a QSO.
const sb = {};
vm.createContext(sb);
vm.runInContext(extractFunction(src, 'txLogsQso'), sb);
for (const x of ['K1ABC KB2SLO RR73', 'K1ABC KB2SLO 73', 'K1ABC KB2SLO RRR', ' K1ABC KB2SLO RR73 ']) {
  assert.strictEqual(sb.txLogsQso(x), true, 'logs a QSO: ' + JSON.stringify(x));
}
for (const x of ['CQ KB2SLO EM10', 'K1ABC KB2SLO EM10', 'K1ABC KB2SLO -07', 'K1ABC KB2SLO R-07', '', null, undefined]) {
  assert.strictEqual(sb.txLogsQso(x), false, 'logs nothing: ' + JSON.stringify(x));
}

// A refresh asked for while one runs is not dropped: it runs again once the first is done.
async function overlap() {
  let fetches = 0, release;
  const timers = [];
  const box = {
    logLoadedOnce: true, logLoading: false, logRefreshing: false, logRefreshAgain: false, currentView: 'log',
    logFiles: ['20261109.adi'], logFileLoadedCount: {}, LOG_ENTRY_FETCH: 10, rows: [],
    checkForNewDayFiles: async () => {},
    appendLogRows: (file, rows) => { box.rows.push(...rows); },
    fetch: () => { fetches++; return new Promise((res) => { release = res; }); },
    // The read's own timeout (logFetchJson(), test_log_timeout.js) is not this test's: only the re-run is kept.
    setTimeout: (fn, ms) => { if (!ms) timers.push(fn); },
    clearTimeout: () => {},
    AbortController, LOG_FETCH_TIMEOUT_MS: 10000,
    encodeURIComponent,
  };
  vm.createContext(box);
  vm.runInContext(extractFunction(src, 'logFetchJson') + '\n' + extractFunction(src, 'refreshLogHead'), box);
  const first = box.refreshLogHead();
  await new Promise((r) => setImmediate(r));
  assert.strictEqual(fetches, 1, 'the first refresh reads today\'s file');
  box.refreshLogHead();  // the QSO is written while the first read is in flight
  assert.strictEqual(fetches, 1, 'no second read at once');
  release({ json: async () => ({ ok: true, entries: [] }) });
  await first;
  assert.strictEqual(timers.length, 1, 'the refresh asked for meanwhile is run again after');
  timers[0]();
  await new Promise((r) => setImmediate(r));
  assert.strictEqual(fetches, 2, 'and reads the file again');
  release({ json: async () => ({ ok: true, entries: [{ call: 'K1ABC' }] }) });
  await new Promise((r) => setImmediate(r));
  await new Promise((r) => setImmediate(r));
  assert.deepStrictEqual(box.rows.map((r) => r.call), ['K1ABC'], 'so the QSO written meanwhile shows');
}

// The wiring, pinned on the source as test_decode_tap.js pins tryReply(): a transmission that turns active
// with a message that logs a QSO starts a refresh, and so does opening the QSOs tab once it has loaded.
assert(/if \(e\.active && !\(txHud && txHud\.active\)\) \{\s*txKeyedAt = Date\.now\(\);\s*if \(txLogsQso\(e\.x\)\) kickLogRefresh\(\);/.test(src),
  'a transmission that logs a QSO starts a refresh as it begins');
assert(/\} else if \(name === 'log'\) \{\s*refreshLogHead\(\);/.test(src), 'opening the QSOs tab refreshes it');

overlap().then(() => console.log('PASS: log refresh on a logged QSO'), (e) => { console.error(e); process.exit(1); });
