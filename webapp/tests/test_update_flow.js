#!/usr/bin/env node
// One tap of Update, as app.html runs it (docs/architecture/webapp.md, Settings › Software update):
// the real runUpdate(), postFwPlan(), followFw() and fwResult() from app.html, with updates.js,
// against a faked sidekick. The web app is part of the sidekick's image, so the page's whole part
// is one request: no app step, no hold of its own. And the start request that does not get
// through is still asked after and sent again.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');
const updates = fs.readFileSync(path.join(__dirname, '..', 'app', 'updates.js'), 'utf8');

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

const V = 'b1322+d18f9b2';
const OLD = 'b1300+aaaaaaa';
const SHA = 'ba9e8126190833eb2c570b5d08c563ae4e04b1ad3977946633935ed7f8f09539';
const ota = (filename, size) => ({ filename, size, sha256: SHA, version: V, proto: 17 });
const MANIFEST = { channel: 'dev', version: V, build: 1322,
  assets: { poe_p4: { ota: ota('p4.bin', 871968) }, sidekick: { ota: ota('sk.bin', 1288160) } } };
const LINK = { hostLinked: true, mainProto: 17, sidekickProto: 17 };

// A sidekick. `start` answers each POST /api/fw/start in turn: 'ok', 'drop' (the request does not
// get through), or a refusal's code. `arrives` makes a dropped request one that arrived all the same.
function world(running, start, arrives) {
  const requests = [], sheets = [];
  let accepted = false, polls = 0, plan = null;
  const res = (status, body) => ({ status, ok: status >= 200 && status < 300, json: async () => body });
  const box = {
    fwMine: null, fwStatus: null, fwReloadOnClose: false, fwStartNote: '', fwPolling: false, fwSheetMode: '',
    FW_FOLLOW_MS: 12 * 60 * 1000,
    renderFwUpdates: () => {},
    checkFwUpdates: () => {},
    fwShow: (mode, o) => { box.fwSheetMode = mode; sheets.push(Object.assign({ mode }, o)); },
    setTimeout: (fn) => { Promise.resolve().then(fn); },
    Date, Promise,
    MicroFT8Pairing: {
      needsReauth: () => {},
      authFetch: async (url, opts) => {
        requests.push((opts && opts.method || 'GET') + ' ' + url);
        assert.strictEqual(url, '/api/fw/start', 'the only request the page sends with the token');
        const how = start.shift() || 'ok';
        if (how === 'drop') {
          if (arrives) accepted = true;
          throw new TypeError('Load failed');
        }
        if (how !== 'ok') return res(409, { ok: false, why: how });
        accepted = true;
        box.sent = opts.body;
        return res(200, { ok: true });
      },
    },
    fetch: async (url) => {
      requests.push('GET ' + url);
      assert.strictEqual(url, '/api/fw/status', 'the only thing the page reads during an update');
      const base = { why: '', at: '', detail: 0, host_linked: true, online: true };
      if (!accepted) return res(200, Object.assign({ state: 'idle', plan: [], step: 0, running: OLD, host: OLD }, base));
      polls++;
      const boards = plan.map((o) => ({ board: o.board, version: o.to, size: o.size }));
      const last = plan[plan.length - 1];
      if (polls < 3) {
        return res(200, Object.assign({ state: 'downloading', board: plan[0].board, version: V, step: 0, got: 1000,
          size: plan[0].size, plan: boards, running: OLD, host: OLD }, base));
      }
      if (last.board === 'main') {
        return res(200, Object.assign({ state: 'done', board: 'main', version: V, step: boards.length - 1, got: 0,
          size: last.size, plan: boards, running: running.sidekick, host: V }, base));
      }
      return res(200, Object.assign({ state: 'idle', board: 'sidekick', version: V, step: boards.length - 1, got: 0,
        size: last.size, plan: boards, running: V, host: V, last: { board: 'sidekick', version: V, from: OLD, result: 'ok' } }, base));
    },
  };
  vm.createContext(box);
  vm.runInContext(updates, box);
  ['runUpdate', 'sendFwPlan', 'fwStartCheck', 'postFwPlan', 'fwOutcome', 'followFw', 'fwResult'].forEach((name) =>
    vm.runInContext(extractFunction(src, name), box));
  plan = vm.runInContext('FwUpdates', box).plan(MANIFEST, running, LINK).steps;
  return {
    box, requests, sheets,
    run: async () => {
      await vm.runInContext('runUpdate(FwUpdates.plan(' + JSON.stringify(MANIFEST) + ', ' + JSON.stringify(running) + ', ' +
        JSON.stringify(LINK) + '))', box);
      for (let i = 0; i < 200 && box.fwSheetMode !== 'result'; i++) await new Promise((r) => setImmediate(r));
      return sheets[sheets.length - 1];
    },
  };
}

(async () => {
  // ---- one tap: one request, then only the sidekick's progress ----
  let w = world({ main: OLD, sidekick: OLD }, ['ok']);
  let end = await w.run();
  const posts = (reqs) => reqs.filter((r) => r.indexOf('POST') === 0);
  assert.deepStrictEqual(posts(w.requests), ['POST /api/fw/start'], 'one request starts the whole update');
  assert.deepStrictEqual([...new Set(w.requests)], ['POST /api/fw/start', 'GET /api/fw/status'],
    'the update\'s own request and its progress, and nothing fetched for the app');
  assert.deepStrictEqual(w.box.sent.split('\n').filter((l) => /^board=|^next$/.test(l)), ['board=main', 'next', 'board=sidekick'],
    'the plan is the boards\' images, the main board\'s first');
  assert.deepStrictEqual([end.mode, end.title, end.failed, end.retry], ['result', 'Updated to b1322', false, false]);
  assert(/Switch the radio back on/.test(end.note) && /Close opens the new app\.$/.test(end.note), end.note);
  assert.deepStrictEqual([w.box.fwMine, w.box.fwPolling, w.box.fwReloadOnClose], [null, false, true]);
  // The progress never lists a step for the app.
  const progress = w.sheets.filter((s) => s.mode === 'progress' && s.steps.length);
  assert(progress.length > 0, 'progress was drawn');
  progress.forEach((s) => s.steps.forEach((r) => assert(!/app/i.test(r.text), 'no app step: ' + r.text)));
  // Values made in the vm are another realm's: compare them as text.
  assert.strictEqual(progress[0].steps.slice(0, 2).map((r) => r.text).join(' | '), 'Download the main board’s image (852 KB) | Verify it');

  // ---- the main board alone: no new app to open ----
  w = world({ main: OLD, sidekick: V }, ['ok']);
  end = await w.run();
  assert.deepStrictEqual([end.title, end.failed], ['Updated to b1322', false]);
  assert(!/new app/.test(end.note), 'the app is the sidekick\'s, which did not change: ' + end.note);

  // ---- a start request that does not get through is asked after, then sent once more ----
  w = world({ main: OLD, sidekick: OLD }, ['drop', 'ok']);
  end = await w.run();
  assert.deepStrictEqual(posts(w.requests).length, 2, 'sent again');
  assert.strictEqual(w.requests[1], 'GET /api/fw/status', 'after asking the sidekick what it is doing');
  assert.deepStrictEqual([end.title, end.failed], ['Updated to b1322', false]);
  assert(/The start request was sent twice: the first did not get through \(Load failed\)\.$/.test(end.note), end.note);

  // It arrived after all: the sidekick is carrying the plan out, and it is not sent again.
  w = world({ main: OLD, sidekick: OLD }, ['drop'], true);
  end = await w.run();
  assert.deepStrictEqual(posts(w.requests).length, 1, 'not sent twice');
  assert.deepStrictEqual([end.title, end.failed], ['Updated to b1322', false]);
  assert(/got no answer \(Load failed\), but the sidekick had it\.$/.test(end.note), end.note);

  // Neither try gets through.
  w = world({ main: OLD, sidekick: OLD }, ['drop', 'drop']);
  end = await w.run();
  assert.deepStrictEqual([posts(w.requests).length, end.title, end.failed, end.retry], [2, 'The sidekick did not answer', true, true]);
  assert.deepStrictEqual([w.box.fwMine, w.box.fwPolling, w.box.fwReloadOnClose], [null, false, false]);

  // ---- a refusal is the result, with Retry, and nothing is followed ----
  w = world({ main: OLD, sidekick: OLD }, ['confirming']);
  end = await w.run();
  assert.deepStrictEqual([end.title, end.failed, end.retry],
    ['The last update is still being confirmed; try again in a minute', true, true]);
  assert.deepStrictEqual(w.requests, ['POST /api/fw/start']);

  console.log('ok (update flow)');
})().catch((e) => { console.error(e); process.exit(1); });
