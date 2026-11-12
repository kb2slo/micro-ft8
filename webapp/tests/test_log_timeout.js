#!/usr/bin/env node
// A log read that never settles does not stop the log (docs/architecture/webapp.md, Screens: QSOs).
// Runs app.html's own logFetchJson() and the functions that hold a busy flag across it, with a fetch
// that behaves as a browser's does when the sidekick restarts under it: no answer, or headers and no
// body, until the signal aborts it. After the timeout each flag is clear and the next ask reads again.
// It cannot see whether a given browser really rejects such a fetch on abort: that is the field check.
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

const TIMEOUT_MS = Number(/var LOG_FETCH_TIMEOUT_MS = (\d+);/.exec(src)[1]);
assert(TIMEOUT_MS >= 6000 && TIMEOUT_MS <= 15000,
  'longer than the 5 s the sidekick waits on the main board, and not so long the list looks stuck');

const settle = async () => { for (let i = 0; i < 6; i++) await new Promise((r) => setImmediate(r)); };
// Rows built inside the vm are another realm's arrays, so they are copied (Array.from) before comparing.
const abortError = () => Object.assign(new Error('aborted'), { name: 'AbortError' });

// A page's globals around the functions under test: a clock whose timers run only when advanced,
// and a fetch whose answers the test scripts. mode: 'hang' never answers; 'body' answers with
// headers and a body that never ends; anything else is the JSON to answer with.
function page(names, extra) {
  const timers = [];
  let now = 0, nextId = 1;
  const box = Object.assign({
    LOG_FETCH_TIMEOUT_MS: TIMEOUT_MS, AbortController, encodeURIComponent,
    setTimeout: (fn, ms) => { const id = nextId++; timers.push({ id, at: now + (ms || 0), fn }); return id; },
    clearTimeout: (id) => { const k = timers.findIndex((t) => t.id === id); if (k >= 0) timers.splice(k, 1); },
    urls: [], answers: [],
    fetch: (url, opts) => {
      box.urls.push(url);
      assert.strictEqual(opts.cache, 'no-store', 'a log read is never served from cache');
      const mode = box.answers.length ? box.answers.shift() : 'hang';
      const untilAbort = () => new Promise((res, rej) => {
        opts.signal.addEventListener('abort', () => rej(abortError()));
      });
      if (mode === 'hang') return untilAbort();
      if (mode === 'body') return Promise.resolve({ json: untilAbort });
      return Promise.resolve({ json: async () => mode });
    },
  }, extra || {});
  box.advance = async (ms) => {
    now += ms;
    for (;;) {
      const due = timers.filter((t) => t.at <= now).sort((a, b) => a.at - b.at)[0];
      if (!due) break;
      timers.splice(timers.indexOf(due), 1);
      due.fn();
      await settle();
    }
    await settle();
  };
  box.pendingTimers = () => timers.length;
  vm.createContext(box);
  vm.runInContext(['logFetchJson'].concat(names).map((n) => extractFunction(src, n)).join('\n'), box);
  return box;
}

async function helper() {
  const p = page([]);
  // An answer in time: the JSON, and no timer left to abort a later read.
  p.answers.push({ ok: true, files: ['20261111.adi'] });
  assert.deepStrictEqual((await p.logFetchJson('/api/log/files?skip=0&take=5')).files, ['20261111.adi']);
  assert.strictEqual(p.pendingTimers(), 0, 'an answered read leaves no timer running');
  // No answer at all, and headers with no body: each rejects at the timeout, not before.
  for (const mode of ['hang', 'body']) {
    p.answers.push(mode);
    let outcome = 'pending';
    p.logFetchJson('/api/log/files?skip=0&take=5').then(() => { outcome = 'answered'; }, () => { outcome = 'rejected'; });
    await p.advance(TIMEOUT_MS - 1);
    assert.strictEqual(outcome, 'pending', mode + ': still waiting just before the timeout');
    await p.advance(1);
    assert.strictEqual(outcome, 'rejected', mode + ': given up at the timeout');
    assert.strictEqual(p.pendingTimers(), 0);
  }
}

// The QSOs list's 8 s refresh: the 2026-11-11 freeze. checkForNewDayFiles() reads first, so it is the
// read that hangs; then the entries read hangs on a later tick.
async function qsosRefresh() {
  for (const hungRead of ['files', 'entries']) {
    const p = page(['checkForNewDayFiles', 'refreshLogHead'], {
      logLoadedOnce: true, logLoading: false, logRefreshing: false, logRefreshAgain: false, currentView: 'log',
      logFiles: ['20261111.adi'], logFileIdx: 0, logFilesSkip: 1, logFileLoadedCount: {}, LOG_ENTRY_FETCH: 10,
      rows: [],
    });
    p.appendLogRows = (file, rows) => { p.rows.push(...rows); };
    if (hungRead === 'entries') p.answers.push({ ok: true, files: ['20261111.adi'] });
    p.answers.push('hang');
    p.refreshLogHead();
    await settle();
    assert.strictEqual(p.logRefreshing, true, hungRead + ': a refresh is waiting on its read');
    const asked = p.urls.length;
    await p.advance(8000);
    p.refreshLogHead();           // the next 8 s tick, while the read still hangs
    await settle();
    assert.strictEqual(p.urls.length, asked, hungRead + ': no second read while one is in flight');
    await p.advance(TIMEOUT_MS);  // the read is given up, and the tick that asked meanwhile runs again
    assert(p.urls.length > asked, hungRead + ': the refresh asked for meanwhile reads again');
    // That read hangs too (nothing scripted): give it up, then the sidekick is back.
    await p.advance(TIMEOUT_MS);
    assert.strictEqual(p.logRefreshing, false, hungRead + ': the flag is clear once the read is given up');
    p.answers.push({ ok: true, files: ['20261111.adi'] }, { ok: true, entries: [{ call: 'K1ABC' }] });
    p.refreshLogHead();
    await settle();
    assert.deepStrictEqual(p.rows.map((r) => r.call), ['K1ABC'], hungRead + ': a QSO logged after the restart shows');
    assert.strictEqual(p.logRefreshing, false);
  }
}

// Operate's read of today's file, for the Worked flag and the activation line.
async function operateLog() {
  const p = page(['refreshOpLog'], {
    opLog: { file: '', rows: [], busy: false }, currentView: 'operate', lastCfg: {}, drawn: 0,
    utcDayFile: () => '20261111.adi', parseActRefs: () => [],
  });
  p.renderDecodes = () => { p.drawn++; };
  p.renderActLine = () => {};
  p.refreshOpLog();
  await settle();
  assert.strictEqual(p.opLog.busy, true);
  await p.advance(TIMEOUT_MS);
  assert.strictEqual(p.opLog.busy, false, 'Operate\'s log read is given up at the timeout');
  p.answers.push({ ok: true, entries: [{ call: 'K1ABC' }] });
  await p.refreshOpLog();
  assert.deepStrictEqual(Array.from(p.opLog.rows, (r) => r.call), ['K1ABC'], 'and the next tick reads the QSO');
  assert.strictEqual(p.drawn, 1, 'which redraws the decodes for the Worked flag');
}

// Load older QSOs: the button comes back, and the list says why it is empty.
async function loadOlder() {
  const el = { logMoreBtn: { disabled: false }, logMore: { hidden: false }, logList: { innerHTML: '' } };
  const p = page(['ensureLogFiles', 'loadMoreLog'], {
    logLoading: false, logDone: false, logRenderedAny: false, LOG_ENTRY_GOAL: 40, LOG_ENTRY_FETCH: 10,
    logFiles: [], logFileIdx: 0, logFilesSkip: 0, logFilesExhausted: false, logFileLoadedCount: {}, rows: [],
    document: { getElementById: (id) => el[id] },
  });
  p.appendLogRows = (file, rows) => { p.rows.push(...rows); p.logRenderedAny = true; };
  p.loadMoreLog();
  await settle();
  assert.strictEqual(p.logLoading, true);
  assert.strictEqual(el.logMoreBtn.disabled, true);
  await p.advance(TIMEOUT_MS);
  assert.strictEqual(p.logLoading, false, 'the load is given up at the timeout');
  assert.strictEqual(el.logMoreBtn.disabled, false, 'and its button works again');
  assert(/no reply from sidekick/.test(el.logList.innerHTML));
  p.answers.push({ ok: true, files: ['20261111.adi'] }, { ok: true, entries: [{ call: 'K1ABC' }] });
  await p.loadMoreLog();
  assert.deepStrictEqual(p.rows.map((r) => r.call), ['K1ABC'], 'a second tap loads the list');
}

// A day's full rows, behind its download link: a read that failed or hung is not kept.
async function dayRows() {
  const p = page(['fetchAllEntries', 'dayFullRows'], { dayFullRowsCache: {} });
  let failed = false;
  p.dayFullRows('20261111.adi').catch(() => { failed = true; });
  await p.advance(TIMEOUT_MS);
  assert.strictEqual(failed, true, 'the hung read fails at the timeout');
  assert.strictEqual(p.dayFullRowsCache['20261111.adi'], undefined, 'and is not kept');
  p.answers.push({ ok: true, entries: [{ call: 'K1ABC' }] });
  assert.deepStrictEqual(Array.from(await p.dayFullRows('20261111.adi'), (r) => r.call), ['K1ABC'], 'the next ask reads again');
  assert.strictEqual(p.dayFullRows('20261111.adi'), p.dayFullRowsCache['20261111.adi'], 'a read that worked is kept');
  assert.strictEqual(p.urls.length, 2);
}

// No log read in the app goes around the helper.
assert(!/fetch\(\s*'\/api\/log\/(files|entries)\?/.test(src.replace(/logFetchJson\(/g, 'L(')),
  'every read of /api/log/files and /api/log/entries in app.html goes through logFetchJson()');

helper().then(qsosRefresh).then(operateLog).then(loadOlder).then(dayRows)
  .then(() => console.log('PASS: a log read that never settles is given up'), (e) => { console.error(e); process.exit(1); });
