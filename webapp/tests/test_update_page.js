#!/usr/bin/env node
// The sidekick's /update page (sidekick/main/web/update.html; docs/architecture/sidekick.md, Firmware
// updates), its own script run against a faked sidekick and release host: what it posts, and what it
// says as an update runs and ends. Not the page's look, nor a real sidekick.
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const html = fs.readFileSync(path.join(__dirname, '..', '..', 'sidekick', 'main', 'web', 'update.html'), 'utf8');
const script = /<script>([\s\S]*?)<\/script>/.exec(html)[1];

const SHA = 'a'.repeat(64);
const MANIFEST = {
  channel: 'dev',
  assets: {
    poe_p4: { ota: { filename: 'p4.bin', size: 900000, sha256: SHA, version: 'b1348+1203a59' } },
    sidekick: { ota: { filename: 'sk.bin', size: 1200000, sha256: SHA, version: 'b1348+1203a59' } },
  },
};

// statuses: what each GET /api/fw/status answers in turn, the last repeated; 'down' is no answer.
function world(statuses) {
  const el = { m: { textContent: '' }, go: { disabled: true, onclick: null } };
  const said = [];
  const posts = [];
  let now = 0, polls = 0;
  const res = (body, status) => ({ ok: (status || 200) < 300, status: status || 200,
    json: async () => body, text: async () => JSON.stringify(body) });
  const fetch = async (url) => {
    if (url === '/api/fw/status') {
      const s = statuses[Math.min(polls++, statuses.length - 1)];
      if (s === 'down') throw new TypeError('Load failed');
      return res(s);
    }
    if (/manifest\.json$/.test(url)) return res(MANIFEST);
    throw new Error('unexpected fetch ' + url);
  };
  const ctx = {
    document: { getElementById: (id) => el[id] },
    MicroFT8Pairing: {
      getToken: () => 'tok', needsReauth() {}, boot() {},
      authFetch: async (url, o) => { posts.push([url, o.body]); return res({ ok: true }); },
    },
    fetch, AbortController,
    // Time passes only as the page waits, so a run takes no real time.
    setTimeout: (fn, ms) => { now += ms; setImmediate(fn); return 0; },
    clearTimeout() {},
    Date: { now: () => now },
    Promise,
  };
  vm.createContext(ctx);
  vm.runInContext(script, ctx);
  const m = el.m;
  let last = '';
  return {
    el, said, posts,
    async tap() {
      el.go.onclick();
      for (let i = 0; i < 500; i++) {
        await new Promise((r) => setImmediate(r));
        if (m.textContent !== last) { last = m.textContent; said.push(last); }
        if (!el.go.disabled && said.length > 1) break;
      }
      return last;
    },
  };
}

const base = { why: '', at: '', got: 0, size: 0, board: 'main', plan: [], running: '1203a59', host: 'b1343+a46ca06',
  host_linked: true, online: true };
const st = (o) => Object.assign({}, base, o);

(async () => {
  // Both boards differ: one request, the main board's image first, then the sidekick's.
  let w = world([st({ state: 'idle' }), st({ state: 'downloading', got: 450000, size: 900000 }),
    st({ state: 'transferring', got: 450000, size: 900000 }), st({ state: 'restarting' }),
    st({ state: 'downloading', board: 'sidekick', got: 600000, size: 1200000 }),
    st({ state: 'restarting', board: 'sidekick' }),
    // The sidekick restarted between two polls: it answers idle, and no poll failed.
    st({ state: 'idle', running: 'b1348+1203a59', last: { result: 'ok' } })]);
  let end = await w.tap();
  assert.strictEqual(w.posts.length, 1, 'one request');
  assert.strictEqual(w.posts[0][0], '/api/fw/start');
  assert.deepStrictEqual(w.posts[0][1].split('\n').filter((l) => /^board=|^next$/.test(l)), ['board=main', 'next', 'board=sidekick']);
  assert.strictEqual(end, 'Done. Switch the radio back on, then open the app.', 'ends on Done with no failed poll');
  assert.deepStrictEqual(w.said.filter((t) => !/^(Reading|Started)/.test(t)).slice(0, -1), ['Downloading the main board’s image 50%',
    'Sending the image to the main board 50%', 'Restarting the main board',
    'Downloading the sidekick’s image 50%', 'Restarting the sidekick']);

  // The restart seen as no answer, then idle.
  w = world([st({ state: 'idle' }), st({ state: 'restarting', board: 'sidekick' }), 'down',
    st({ state: 'idle', last: { result: 'ok' } })]);
  assert.strictEqual(await w.tap(), 'Done. Switch the radio back on, then open the app.');

  // Rolled back, and failed.
  w = world([st({ state: 'idle' }), st({ state: 'restarting', board: 'sidekick' }), st({ state: 'idle', last: { result: 'rolled_back' } })]);
  assert(/went back/.test(await w.tap()));
  w = world([st({ state: 'idle' }), st({ state: 'failed', why: 'hash', at: 'verifying' })]);
  assert.strictEqual(await w.tap(), 'The update failed: hash (verifying). Tap Install to try again.');

  // Idle from the start: no update ever seen running is not taken for a finished one.
  w = world([st({ state: 'idle' })]);
  assert(/no update running/.test(await w.tap()));

  // Nothing to install, no main board, no internet: no request.
  for (const [s, re] of [[{ host: 'b1348+1203a59', running: 'b1348+1203a59' }, /already runs/],
    [{ host_linked: false, host: '' }, /main board connected/], [{ online: false }, /own network/]]) {
    w = world([st(Object.assign({ state: 'idle' }, s))]);
    assert(re.test(await w.tap()), re);
    assert.strictEqual(w.posts.length, 0, String(re));
  }
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
