#!/usr/bin/env node
// The live polls never pile up (roadmap row uwzn): tick() calls pollEvents() every second and pollLiveStatus()
// runs every 5 s, whether or not the last one answered. Runs app.html's own two functions with a fetch that
// never answers until its signal aborts: while one is out, the next call asks nothing; at the timeout it is
// given up, and the next call asks again. Whether a given browser really rejects on abort is not seen here.
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

function constant(name) {
  const m = new RegExp('var ' + name + ' = (\\d+);').exec(src);
  assert(m, name + ' not found in app.html');
  return Number(m[1]);
}
const EVENTS_MS = constant('LIVE_POLL_TIMEOUT_MS');
const STATUS_MS = constant('STATUS_POLL_TIMEOUT_MS');
assert(EVENTS_MS >= 5000 && EVENTS_MS < 20000,
  'long enough not to cut a slow answer in a stall, and shorter than the sidekick\'s 20 s send deadline');
assert(STATUS_MS > 5000, 'longer than the 5 s a config read waits on the main board');

const settle = async () => { for (let i = 0; i < 6; i++) await new Promise((r) => setImmediate(r)); };
const abortError = () => Object.assign(new Error('aborted'), { name: 'AbortError' });

// A page's globals around the two polls: timers that run only when advanced, a fetch the test answers,
// and the rest of the page as no-ops.
function page() {
  const timers = [];
  let now = 0, nextId = 1;
  const noop = () => {};
  const box = {
    LIVE_POLL_TIMEOUT_MS: EVENTS_MS, STATUS_POLL_TIMEOUT_MS: STATUS_MS, AbortController,
    evPolling: false, cfgPolling: false, evSeq: 5, DECODE_SLOTS_MAX: 8, holdCardOpen: false,
    lastCfg: null, lastConfigOkMs: 0, lastCfgAt: 0, Date: { now: () => now },
    setTimeout: (fn, ms) => { const id = nextId++; timers.push({ id, at: now + (ms || 0), fn }); return id; },
    clearTimeout: (id) => { const k = timers.findIndex((t) => t.id === id); if (k >= 0) timers.splice(k, 1); },
    urls: [], answers: [],
    fetch: (url, opts) => {
      box.urls.push(url);
      assert.strictEqual(opts.cache, 'no-store');
      const mode = box.answers.length ? box.answers.shift() : 'hang';
      if (mode === 'hang') {
        return new Promise((res, rej) => opts.signal.addEventListener('abort', () => rej(abortError())));
      }
      return Promise.resolve({ ok: true, json: async () => mode });
    },
    applyEvent: noop, rebuildQueueOrderFromEvents: noop, flushDecodeBatches: noop, renderDecodes: noop,
    sweepStaleQueue: () => false, renderQueue: noop, renderCqRow: noop, renderRadioBar: noop,
    saveStateCache: noop, updateResetHold: noop, startHoldReset: noop, resetLiveState: noop,
    noteSidekickAnswer: noop, applySidekickFacts: noop, setMainBoardLive: noop, applyOperateLive: noop,
    autoSyncClock: noop, PskrLive: { sync: noop },
  };
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
  vm.runInContext(['pollEvents', 'pollLiveStatus'].map((n) => extractFunction(src, n)).join('\n'), box);
  return box;
}

async function oneAtATime(fn, url, timeoutMs, flag, answer) {
  const p = page();
  p[fn]();
  await settle();
  assert.deepStrictEqual(p.urls, [url], fn + ': the first call asks');
  assert.strictEqual(p[flag], true);
  // Every second until just before the timeout, as tick() does: nothing more is asked.
  for (let t = 1000; t < timeoutMs; t += 1000) {
    await p.advance(1000);
    p[fn]();
    await settle();
  }
  assert.strictEqual(p.urls.length, 1, fn + ': no second request while the first is out');
  // The timeout gives it up, and the next call asks again.
  await p.advance(timeoutMs - (Math.floor((timeoutMs - 1) / 1000) * 1000));
  assert.strictEqual(p[flag], false, fn + ': given up at the timeout');
  assert.strictEqual(p.pendingTimers(), 0);
  p.answers.push(answer);
  p[fn]();
  await settle();
  assert.strictEqual(p.urls.length, 2, fn + ': the next call after the timeout asks again');
  assert.strictEqual(p[flag], false, fn + ': an answered poll clears its flag');
  assert.strictEqual(p.pendingTimers(), 0, fn + ': an answered poll leaves no timer running');
  // And one that answers at once never blocks the next.
  p.answers.push(answer);
  p[fn]();
  await settle();
  assert.strictEqual(p.urls.length, 3);
}

(async () => {
  await oneAtATime('pollEvents', '/api/events?since=5', EVENTS_MS, 'evPolling',
                   { events: [], seq: 5, hold: 0, hold_of: 10 });
  await oneAtATime('pollLiveStatus', '/api/config', STATUS_MS, 'cfgPolling', { ok: true });
  console.log('test_live_poll: ok');
})().catch((e) => { console.error(e); process.exit(1); });
