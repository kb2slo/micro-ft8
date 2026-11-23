#!/usr/bin/env node
// pskr_report_core.js (docs/architecture/webapp.md, Spotting): which decodes are spots, what a page holds
// and posts, and when, against a stand-in for the site's /pskr/report route answering as worker.js does.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const ctx = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'pskr_report_core.js'), 'utf8'), ctx);
const R = vm.runInContext('PskrReport', ctx);
const plain = (x) => JSON.parse(JSON.stringify(x));
const MIN = 60 * 1000;
const T0 = 1795000000;

// The sender of a message as app.html's extractSenderCall() gives it, for the shapes used here.
const sender = (x) => {
  const w = x.trim().toUpperCase().split(/\s+/);
  const s = w[0] === 'CQ' ? (/^[A-R]{2}[0-9]{2}$/.test(w[w.length - 1]) ? w[w.length - 2] : w[w.length - 1]) : w[1];
  return /\d/.test(s || '') ? s : '';
};
const ev = (x, more) => Object.assign({ d: 1, x, snr: -12, hz: 1500, fq: 14074000, sl: 150, t: T0 }, more);

// ---- Which decodes are spots ----
assert.deepStrictEqual(plain(R.spotOf(ev('CQ K1ABC FN42'), sender)), { c: 'K1ABC', g: 'FN42', f: 14075500, s: -12, m: 'FT8', t: T0 });
assert.deepStrictEqual(plain(R.spotOf(ev('CQ POTA K1ABC FN42'), sender)).c, 'K1ABC');
assert.deepStrictEqual(plain(R.spotOf(ev('KB2SLO W1AW EM10'), sender)).g, 'EM10', 'a reply with its locator');
assert.strictEqual(R.spotOf(ev('KB2SLO W1AW RR73'), sender).g, '', 'RR73 is a sign-off, not a locator');
assert.strictEqual(R.spotOf(ev('KB2SLO W1AW -07'), sender).g, '', 'a report');
assert.strictEqual(R.spotOf(ev('KB2SLO W1AW R-07'), sender).g, '');
assert.deepStrictEqual(plain(R.spotOf(ev('CQ K1ABC FN42', { sl: 75, fq: 14080000, hz: 900.4, snr: 3.2 }), sender)),
  { c: 'K1ABC', g: 'FN42', f: 14080900, s: 3, m: 'FT4', t: T0 }, 'FT4, on its own dial');
assert.strictEqual(R.spotOf(ev('CQ K1ABC FN42', { t: 0 }), sender), null, 'no time: the clock is unset');
assert.strictEqual(R.spotOf(ev('CQ K1ABC FN42', { fq: 0 }), sender), null, 'no dial');
assert.strictEqual(R.spotOf(ev('TNX FER QSO'), sender), null, 'free text names no sender');
assert.strictEqual(R.spotOf(null, sender), null);

// ---- A page's reports ----
function page(answer) {
  const clock = { ms: T0 * 1000 };
  const posts = [];
  const r = R.create({
    now: () => clock.ms, random: () => 0.5,
    post: (body) => { posts.push(plain(body)); return answer(body, posts.length); },
  });
  return { r, clock, posts };
}
const on = { on: true, call: 'KB2SLO', grid: 'EM10cg', app: '0.1.0+0c5ccec' };
const spot = (c, t, more) => Object.assign({ c, g: 'FN42', f: 14075500, s: -10, m: 'FT8', t: t || T0 }, more);

(async function () {
  {
    const { r, clock, posts } = page(() => Promise.resolve({ ok: true, sent: 2, retry_in: 5 * MIN }));
    assert.strictEqual(r.tick(on), null, 'nothing held, nothing posted');
    r.note(spot('K1ABC'));
    r.note(spot('W1AW'));
    r.note(spot('K1ABC', T0 + 15, { s: -3 }));
    assert.strictEqual(r.held(), 2, 'one spot a sender, its latest');
    assert.strictEqual(r.tick(on), null, 'a first post waits a moment, so pages do not post together');
    clock.ms += R.FIRST_MS;
    await r.tick(on);
    assert.strictEqual(posts.length, 1);
    assert.deepStrictEqual(posts[0].spots.map((s) => [s.c, s.s]), [['K1ABC', -3], ['W1AW', -10]], 'newest first');
    assert.deepStrictEqual([posts[0].call, posts[0].grid, posts[0].app], ['KB2SLO', 'EM10cg', '0.1.0+0c5ccec']);
    assert.strictEqual(r.held(), 0, 'what went is forgotten');
    assert.strictEqual(R.statusLine(on, r.last(), r.held(), clock.ms), 'Sent 2 spots, just now.');

    r.note(spot('VE3PFW', T0 + 60));
    clock.ms += 4 * MIN;
    assert.strictEqual(r.tick(on), null, 'not before the window the route named');
    clock.ms += MIN + 20 * 1000;
    await r.tick(on);
    assert.strictEqual(posts.length, 2);
    assert.strictEqual(R.statusLine(on, r.last(), r.held(), clock.ms + 3 * MIN), 'Sent 2 spots, 3 min ago.');
  }

  // Another page sent first: this one keeps its spots and posts when told, and a spot heard meanwhile
  // goes with them.
  {
    const answers = [{ ok: true, sent: 0, retry_in: 3 * MIN }, { ok: true, sent: 1, retry_in: 5 * MIN }];
    const { r, clock, posts } = page(() => Promise.resolve(answers.shift()));
    r.note(spot('K1ABC'));
    clock.ms += R.FIRST_MS;
    await r.tick(on);
    assert.strictEqual(r.held(), 1, 'kept');
    assert.strictEqual(R.statusLine(on, r.last(), r.held(), clock.ms), '1 waiting for the next report.');
    r.note(spot('W1AW', T0 + 30));
    clock.ms += 2 * MIN;
    assert.strictEqual(r.tick(on), null);
    clock.ms += MIN + 20 * 1000;
    await r.tick(on);
    assert.deepStrictEqual(posts[1].spots.map((s) => s.c), ['W1AW', 'K1ABC']);
    assert.strictEqual(r.held(), 0);
  }

  // Every spot already reported: dropped, and the page does not post again for at least a minute.
  {
    const { r, clock, posts } = page(() => Promise.resolve({ ok: true, sent: 0, retry_in: 0 }));
    r.note(spot('K1ABC'));
    clock.ms += R.FIRST_MS;
    await r.tick(on);
    assert.strictEqual(r.held(), 0);
    assert.strictEqual(R.statusLine(on, r.last(), r.held(), clock.ms), 'Up to date, just now.');
    r.note(spot('W1AW', T0 + 30));
    clock.ms += 30 * 1000;
    assert.strictEqual(r.tick(on), null, 'a page posts at most once a minute');
    clock.ms += R.MIN_GAP_MS;
    await r.tick(on);
    assert.strictEqual(posts.length, 2);
  }

  // The site unreachable: kept, and the page backs off. PSKReporter unreachable: dropped.
  {
    let fail = true;
    const { r, clock, posts } = page(() => fail ? Promise.reject(new Error('offline'))
      : Promise.resolve({ ok: false, error: 'unreachable', retry_in: 5 * MIN }));
    r.note(spot('K1ABC'));
    clock.ms += R.FIRST_MS;
    await r.tick(on);
    assert.strictEqual(r.held(), 1, 'kept');
    assert.strictEqual(R.statusLine(on, r.last(), r.held(), clock.ms), 'Not sent: no internet, or the site did not answer, just now. Trying again.');
    clock.ms += R.BACKOFF_MS[0] - 1000;
    assert.strictEqual(r.tick(on), null, 'backs off');
    clock.ms += 30 * 1000;
    await r.tick(on);
    assert.strictEqual(posts.length, 2);
    clock.ms += R.BACKOFF_MS[1] + 30 * 1000;
    fail = false;
    await r.tick(on);
    assert.strictEqual(r.held(), 0, 'lost on the way to PSKReporter: not posted twice');
    assert.strictEqual(R.statusLine(on, r.last(), r.held(), clock.ms), 'PSKReporter did not answer, just now.');
  }

  // The browser knows it is offline: no attempt, the spots wait, and the page posts once it is back,
  // without waiting out a back-off.
  {
    let online = true, fail = true;
    const clock = { ms: T0 * 1000 };
    const posts = [];
    const r = R.create({ now: () => clock.ms, random: () => 0.5, online: () => online,
      post: (body) => { posts.push(body); return fail ? Promise.reject(new Error('offline')) : Promise.resolve({ ok: true, sent: 1, retry_in: 5 * MIN }); } });
    r.note(spot('K1ABC'));
    clock.ms += R.FIRST_MS;
    await r.tick(on);
    assert.strictEqual(posts.length, 1, 'tried once while the browser thought it was online');
    online = false;
    for (let i = 0; i < 20; i++) { clock.ms += 30 * 1000; assert.strictEqual(r.tick(on), null); }
    assert.strictEqual(posts.length, 1, 'no attempt while offline');
    assert.strictEqual(r.held(), 1, 'the spots wait');
    assert.strictEqual(R.statusLine(on, r.last(), r.held(), clock.ms), 'Not sent: the phone is offline. 1 waiting.');
    online = true;
    fail = false;
    await r.tick(on);
    assert.strictEqual(posts.length, 2, 'back online: posts at once');
    assert.strictEqual(r.held(), 0);
  }

  // Older than PSKReporter keeps: dropped without a post. Turned off: forgotten.
  {
    const { r, clock, posts } = page(() => Promise.resolve({ ok: true, sent: 1, retry_in: 5 * MIN }));
    r.note(spot('K1ABC'));
    clock.ms += (R.MAX_AGE_S + 1) * 1000;
    assert.strictEqual(r.tick(on), null);
    assert.strictEqual(r.held(), 0);
    r.note(spot('W1AW', Math.floor(clock.ms / 1000)));
    assert.strictEqual(r.tick(Object.assign({}, on, { on: false })), null);
    assert.strictEqual(r.held(), 0, 'off: nothing held');
    r.note(spot('W1AW', Math.floor(clock.ms / 1000)));
    assert.strictEqual(r.tick(Object.assign({}, on, { grid: '' })), null, 'no grid, no report');
    assert.strictEqual(posts.length, 0);
    // A post holds at most what the route takes, the newest.
    for (let i = 0; i < R.MAX_HELD + 5; i++) r.note(spot('K' + i + 'AB', Math.floor(clock.ms / 1000) - i));
    clock.ms += R.FIRST_MS;
    await r.tick(on);
    assert.strictEqual(posts[0].spots.length, R.MAX_HELD);
    assert.strictEqual(posts[0].spots[0].c, 'K0AB');
  }

  assert.strictEqual(R.statusLine({ on: false }, null, 0, 0), '');
  assert.strictEqual(R.statusLine({ on: true, call: 'KB2SLO', grid: '' }, null, 0, 0), 'Needs the station\'s call and grid.');
  assert.strictEqual(R.statusLine(on, null, 0, 0), 'Nothing heard yet.');
  assert.strictEqual(R.statusLine(on, null, 3, 0), '3 waiting to send.');
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
