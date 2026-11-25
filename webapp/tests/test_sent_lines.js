#!/usr/bin/env node
// What we sent, in the decodes (docs/architecture/webapp.md, What we sent): runs the real functions from
// app.html, as test_decode_view.js does. A transmission is one line, by its slot, from the TX HUD events: the
// repeats while it is on the air and a replayed event make no second line; it reads Sent once it ends whole,
// Stopped when it ended early (with how far in) or the main board aborted it (with its reason), Not sent when
// the slot was not keyed; a lost falling edge counts as sent; Tune makes none; FT4's half-second periods; the
// lines shown span the decode slots kept, or the newest few on a quiet band; and decode slots and lines sort
// into one list by time.
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
  return text.slice(start, i);
}

// The constants as app.html declares them, so a change there is the one tested.
function extractVar(text, name) {
  const m = text.match(new RegExp('var ' + name + ' = [^;]+;'));
  assert(m, 'var not found in app.html: ' + name);
  return m[0];
}

const sandbox = {};
vm.createContext(sandbox);
// esc() as app.html has it; the extractor reads the quote in its regex as a string's start.
vm.runInContext([extractVar(src, 'SENT_TX_MAX'), extractVar(src, 'SENT_FULL_MS'), extractVar(src, 'SENT_RECENT_MS')].join('\n') + '\n' +
  "function esc(s) { return String(s == null ? '' : s).replace(/[&<>]/g, function (c) {" +
  " return { '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]; }); }\n" +
  ['noteSentTx', 'sentShown', 'sentLineModel', 'fmtUtcSecs', 'decodeListOrder', 'sentLineHtml', 'fmtDuration', 'fmtAgo']
    .map((n) => extractFunction(src, n)).join('\n'), sandbox);
const { noteSentTx, sentShown, sentLineModel, decodeListOrder, sentLineHtml, SENT_TX_MAX } = sandbox;

let failed = 0;
function check(name, fn) {
  try { fn(); console.log('PASS  ' + name); } catch (e) { failed++; console.log('FAIL  ' + name + '\n      ' + e.message); }
}
const plain = (v) => JSON.parse(JSON.stringify(v));

// A slot starting at 14:02:45 UTC, as the station's epoch seconds.
const T0 = Date.UTC(2026, 10, 23, 14, 2, 45) / 1000;
const MSG = 'K1ABC KB2SLO R-05';
const on = (t, x, more) => Object.assign({ d: 4, active: true, x: x || MSG, hz: 1234, t: t }, more || {});
const off = (t) => ({ d: 4, active: false, x: '', t: t });
const ctx = (more) => Object.assign({ prevActive: false, mode: 'FT8', tune: false, nowMs: 0 }, more || {});

// One FT8 transmission as the main board reports it: every 500 ms while keyed (whole seconds on the
// wire), then the falling edge after its tones end.
function sendWhole(list, t0, x, mode) {
  let l = noteSentTx(list, on(t0, x), ctx({ mode: mode }));
  const len = mode === 'FT4' ? 5 : 13;
  for (let s = 1; s <= len; s++) l = noteSentTx(l, on(t0 + s, x), ctx({ prevActive: true, mode: mode }));
  return noteSentTx(l, off(t0 + len + 1), ctx({ prevActive: true, mode: mode }));
}

check('a whole transmission is one line, Sent, in its slot', () => {
  const l = plain(sendWhole([], T0, MSG));
  assert.strictEqual(l.length, 1);
  assert.deepStrictEqual(l[0], { x: MSG, hz: 1234, md: 'FT8', ts: (T0 + 15) * 1000, lenMs: 15000, st: 'sent', reason: '', t0: T0 * 1000 + 500, lapsed: false });
  const m = plain(sentLineModel(l[0], false));
  assert.deepStrictEqual(m, { label: 'Sent', note: '', time: '14:02:45z' });
});

check('while on the air it reads Sending', () => {
  const l = noteSentTx([], on(T0 + 1), ctx());
  assert.strictEqual(sentLineModel(l[0], false).label, 'Sending');
});

check('an event replayed in the same slot makes no second line', () => {
  let l = noteSentTx([], on(T0), ctx());
  l = noteSentTx(l, on(T0 + 4), ctx({ prevActive: false }));  // /api/state after a reload: prevActive is lost
  assert.strictEqual(l.length, 1);
});

check('a Stop part way through reads Stopped, with how far in', () => {
  let l = noteSentTx([], on(T0), ctx());
  l = noteSentTx(l, on(T0 + 3), ctx({ prevActive: true }));
  l = noteSentTx(l, off(T0 + 6), ctx({ prevActive: true }));
  assert.strictEqual(l[0].st, 'stopped');
  assert.deepStrictEqual(plain(sentLineModel(l[0], false)), { label: 'Stopped', note: 'Ended 6 s in', time: '14:02:45z' });
});

check('an end just past the threshold is still Sent; just before it, Stopped', () => {
  let l = noteSentTx([], on(T0), ctx());
  assert.strictEqual(noteSentTx(l, off(T0 + 11), ctx({ prevActive: true }))[0].st, 'sent');
  assert.strictEqual(noteSentTx(l, off(T0 + 10), ctx({ prevActive: true }))[0].st, 'stopped');
});

check('an abort by the main board reads Stopped with its reason', () => {
  let l = noteSentTx([], on(T0), ctx());
  l = noteSentTx(l, on(T0 + 2, MSG, { aborted: true, reason: 'SWR protection' }), ctx({ prevActive: true }));
  l = noteSentTx(l, off(T0 + 15), ctx({ prevActive: true }));  // the abort shows to the slot's end; then it falls
  assert.strictEqual(l[0].st, 'stopped');
  assert.strictEqual(sentLineModel(l[0], false).note, 'SWR protection');
});

check('a slot not keyed reads Not sent, Not counted with the reason', () => {
  const l = noteSentTx([], on(T0, MSG, { nk: true, reason: 'CAT not ready' }), ctx());
  assert.deepStrictEqual(plain(sentLineModel(l[0], false)),
    { label: 'Not sent', note: 'Not counted: CAT not ready', time: '14:02:45z' });
  assert.strictEqual(noteSentTx(l, off(T0 + 15), ctx({ prevActive: true }))[0].st, 'notkeyed');
});

check('a lost falling edge counts as sent, at the next transmission or when the HUD goes stale', () => {
  let l = noteSentTx([], on(T0), ctx());
  l = noteSentTx(l, on(T0 + 30, 'K1ABC KB2SLO RR73'), ctx({ prevActive: true }));  // the next slot of our parity
  assert.deepStrictEqual(l.map((s) => s.st), ['sent', 'on']);
  l = noteSentTx(l, { active: false }, {});  // expireStaleTxHud(): no time on it
  assert.deepStrictEqual(l.map((s) => s.st), ['sent', 'sent']);
});

check('a HUD gone stale while still keyed reopens at its next event, and its falling edge decides', () => {
  // Bench, 2026-11-23: our 73 to WE8M read Sent 5 s into a 13 s transmission, the page's polls held up
  // while it read the log the 73 wrote.
  let l = noteSentTx([], on(T0), ctx());
  l = noteSentTx(l, on(T0 + 2), ctx({ prevActive: true }));
  l = noteSentTx(l, { active: false }, {});                         // expireStaleTxHud()
  assert.strictEqual(sentLineModel(l[0], false).label, 'Sent');
  l = noteSentTx(l, on(T0 + 6), ctx({ prevActive: false }));        // the poll catches up
  assert.strictEqual(sentLineModel(l[0], false).label, 'Sending');
  assert.strictEqual(noteSentTx(l, off(T0 + 13), ctx({ prevActive: true }))[0].st, 'sent');
  // Lapsed, and its falling edge then says it ended early: Stopped.
  let s = noteSentTx(noteSentTx([], on(T0), ctx()), { active: false }, {});
  s = noteSentTx(s, off(T0 + 6), ctx({ prevActive: false }));
  assert.deepStrictEqual([s[0].st, sentLineModel(s[0], false).note], ['stopped', 'Ended 6 s in']);
  // Lapsed and never heard from again: Sent, and the next transmission leaves it so.
  let u = noteSentTx(noteSentTx([], on(T0), ctx()), { active: false }, {});
  u = noteSentTx(u, on(T0 + 30, 'K1ABC KB2SLO RR73'), ctx());
  assert.deepStrictEqual(u.map((x) => x.st), ['sent', 'on']);
  assert.strictEqual(u[0].lapsed, false);
});

check('a CQ keyed before its queue entry arrives takes its mode, and period, from the events after', () => {
  // Bench, 2026-11-23: an FT4 CQ keyed at 15:40:15, read as FT8 from the slot state until its entry
  // arrived, and its whole 5 s transmission read Stopped against FT8's 11 s.
  let l = noteSentTx([], on(T0 + 1, 'CQ KB2SLO EM10'), ctx({ mode: 'FT8' }));   // slot state's guess
  l = noteSentTx(l, on(T0 + 2, 'CQ KB2SLO EM10'), ctx({ prevActive: true, mode: 'FT4' }));
  for (let s = 3; s <= 5; s++) l = noteSentTx(l, on(T0 + s, 'CQ KB2SLO EM10'), ctx({ prevActive: true, mode: 'FT4' }));
  l = noteSentTx(l, off(T0 + 6), ctx({ prevActive: true, mode: 'FT4' }));
  assert.strictEqual(l.length, 1);
  assert.deepStrictEqual(plain(sentLineModel(l[0], true)), { label: 'Sent FT4', note: '', time: '14:02:45z' });
  assert.strictEqual(l[0].ts, (T0 + 7.5) * 1000);
});

check('Tune makes no line, nor an event with no text', () => {
  assert.strictEqual(noteSentTx([], on(T0, 'TUNE'), ctx()).length, 0);
  assert.strictEqual(noteSentTx([], on(T0, MSG), ctx({ tune: true })).length, 0);
  assert.strictEqual(noteSentTx([], on(T0, MSG, { x: '' }), ctx()).length, 0);
});

check('with the clock unset, the rising edge starts a line at the page\'s time', () => {
  const now = Date.UTC(2026, 10, 23, 14, 2, 46);
  let l = noteSentTx([], on(0), ctx({ nowMs: now }));
  l = noteSentTx(l, on(0), ctx({ prevActive: true, nowMs: now + 500 }));
  l = noteSentTx(l, off(0), ctx({ prevActive: true }));
  assert.strictEqual(l.length, 1);
  assert.strictEqual(l[0].ts, (T0 + 15) * 1000);
  assert.strictEqual(l[0].st, 'sent');  // no time on its end: not called stopped
});

check('an FT4 period on the half second, and the mode named while both are listened to', () => {
  const t = T0 + 7;  // 14:02:52, still in the period 45.0-52.5
  const l = sendWhole([], t + 1, 'CQ KB2SLO EM10', 'FT4');  // keyed at 14:02:53: the period 52.5-60.0
  assert.strictEqual(l[0].ts, (T0 + 15) * 1000);
  assert.strictEqual(l[0].lenMs, 7500);
  assert.strictEqual(l[0].st, 'sent');
  assert.deepStrictEqual(plain(sentLineModel(l[0], true)), { label: 'Sent FT4', note: '', time: '14:02:52.5z' });
  // An FT4 Stop 2 s in is short of its 3.5 s threshold.
  let s = noteSentTx([], on(t + 1, 'CQ KB2SLO EM10'), ctx({ mode: 'FT4' }));
  assert.strictEqual(noteSentTx(s, off(t + 2), ctx({ prevActive: true }))[0].st, 'stopped');
});

check('an FT4 period on the half second: its first event, stamped a half second early, is still its line', () => {
  // Bench, 2026-11-23: N3CZ's call keyed at 15:34:22.5, its first event stamped 15:34:22 (whole seconds,
  // rounded down), and the page drew a line in the period before, 15:34:15, with nothing sent in it.
  let l = sendWhole([], T0, 'KB4OLM KB2SLO EM10', 'FT4');          // the period 45.0-52.5
  l = noteSentTx(l, on(T0 + 7, 'N3CZ KB2SLO EM10'), ctx({ mode: 'FT4' }));   // keyed at 52.5, stamped 52
  for (let s = 8; s <= 12; s++) l = noteSentTx(l, on(T0 + s, 'N3CZ KB2SLO EM10'), ctx({ prevActive: true, mode: 'FT4' }));
  l = noteSentTx(l, off(T0 + 13), ctx({ prevActive: true, mode: 'FT4' }));
  assert.deepStrictEqual(plain(l.map((s) => [s.x, sentLineModel(s, false).time, s.st])),
    [['KB4OLM KB2SLO EM10', '14:02:45z', 'sent'], ['N3CZ KB2SLO EM10', '14:02:52.5z', 'sent']]);
});

check('an abort shown to the end of an FT4 period on the whole second stays its line', () => {
  let l = noteSentTx([], on(T0, MSG), ctx({ mode: 'FT4' }));           // the period 45.0-52.5
  l = noteSentTx(l, on(T0 + 2, MSG, { aborted: true, reason: 'SWR protection' }), ctx({ prevActive: true, mode: 'FT4' }));
  l = noteSentTx(l, on(T0 + 7, MSG, { aborted: true, reason: 'SWR protection' }), ctx({ prevActive: true, mode: 'FT4' }));  // 52, +0.5 is the next period
  l = noteSentTx(l, off(T0 + 8), ctx({ prevActive: true, mode: 'FT4' }));
  assert.strictEqual(l.length, 1);
  assert.strictEqual(l[0].st, 'stopped');
});

check('the same message in its parity\'s next period is a new line', () => {
  let l = sendWhole([], T0 + 7, 'CQ KB2SLO EM10', 'FT4');              // keyed at 52.5
  l = sendWhole(l, T0 + 22, 'CQ KB2SLO EM10', 'FT4');                  // keyed at 07.5, 15 s on
  assert.deepStrictEqual(plain(l.map((s) => sentLineModel(s, false).time)), ['14:02:52.5z', '14:03:07.5z']);
});

check('the list keeps the newest few', () => {
  let l = [];
  for (let k = 0; k < SENT_TX_MAX + 3; k++) l = sendWhole(l, T0 + 30 * k, 'CQ KB2SLO EM10');
  assert.strictEqual(l.length, SENT_TX_MAX);
  assert.strictEqual(l[l.length - 1].ts, (T0 + 30 * (SENT_TX_MAX + 2) + 15) * 1000);
});

check('lines shown: back to the oldest decode slot kept, or the newest max on a quiet band', () => {
  const slot = (endS) => Object.assign([], { ts: endS * 1000 });
  const sent = [1, 2, 3, 4, 5].map((k) => ({ ts: (T0 + 30 * k) * 1000 }));
  const kept = [slot(T0 + 135), slot(T0 + 105), slot(T0 + 75)];
  const now = (T0 + 150) * 1000;
  assert.deepStrictEqual(sentShown(sent, kept, 3, now).map((s) => s.ts / 1000 - T0), [90, 120, 150]);
  assert.deepStrictEqual(sentShown(sent, [slot(T0 + 135)], 3, now).map((s) => s.ts / 1000 - T0), [90, 120, 150]);
  assert.deepStrictEqual(sentShown(sent.slice(0, 2), [], 3, now).map((s) => s.ts / 1000 - T0), [30, 60]);
});

check('with fewer decode slots than max, a line shows while recent or while a decode slot that old is kept', () => {
  const slot = (endS) => Object.assign([], { ts: endS * 1000 });
  const sent = [1, 2, 3].map((k) => ({ ts: (T0 + 30 * k) * 1000 }));  // slots ending at T0+30, +60, +90
  const shown = (slots, nowS) => sentShown(sent, slots, 3, nowS * 1000).map((s) => s.ts / 1000 - T0);
  // Three CQs nobody answers, every other slot: all three stay up to the fourth's end.
  assert.deepStrictEqual(shown([], T0 + 120), [30, 60, 90]);
  // The station goes quiet: each line leaves two minutes after its slot ended, and none is left.
  assert.deepStrictEqual(shown([], T0 + 151), [60, 90]);
  assert.deepStrictEqual(shown([], T0 + 211), []);
  assert.deepStrictEqual(shown([], T0 + 8 * 3600), []);  // a phone back from a night's sleep
  // A decode slot from then still on the page keeps the lines since it, however long ago.
  assert.deepStrictEqual(shown([slot(T0 + 45)], T0 + 3600), [60, 90]);
  // A fresh decode slot does not bring back a line from long before it.
  assert.deepStrictEqual(shown([slot(T0 + 3600)], T0 + 3601), []);
});

check('decode slots and lines in one list, newest first, a line above a decode slot ending with it', () => {
  const entry = (endS) => ({ slot: { ts: endS * 1000 } });
  const order = decodeListOrder([entry(75), entry(45), entry(15)], [{ ts: 30000 }, { ts: 60000 }, { ts: 75000 }]);
  assert.deepStrictEqual(order.map((e) => (e.sent ? 'sent ' : 'slot ') + (e.sent ? e.sent.ts : e.slot.ts) / 1000),
    ['sent 75', 'slot 75', 'sent 60', 'slot 45', 'sent 30', 'slot 15']);
});

check('the line\'s markup escapes the message and carries its state', () => {
  const html = sentLineHtml({ x: 'K1ABC <KB2SLO/P> 73', md: 'FT8', ts: (T0 + 15) * 1000, lenMs: 15000, st: 'stopped', endMs: 4000 }, false);
  assert(html.indexOf('&lt;KB2SLO/P&gt;') !== -1, html);
  assert(/class="sentline s-stopped"/.test(html), html);
  assert(/class="sentnote s-stopped">Ended 4 s in</.test(html), html);
});

check('the line\'s time carries its age, counted from its slot\'s end as a decode slot\'s is', () => {
  const end = Date.now() - 12000;
  const html = sentLineHtml({ x: 'CQ KB2SLO EM10', md: 'FT8', ts: end, lenMs: 15000, st: 'sent' }, false);
  assert(html.indexOf('<span class=ago data-ts="' + end + '">12s ago</span>') !== -1, html);
});

if (failed) { console.log(failed + ' failed'); process.exit(1); }
console.log('all passed');
