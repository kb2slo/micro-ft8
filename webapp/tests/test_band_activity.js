#!/usr/bin/env node
// The slot bar's band-activity trace: runs app.html's own noteActivity(), activityBars() and
// activityLabel(), so ACTIVITY events (d 8) land in the right slot and block, a later slot starts
// over, an earlier one is dropped, bars and the "≈N signals" label show only for the slot the bar
// is showing, and the label reads the signals' span, not their edges.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');

function extractFunction(text, name) {
  const start = text.indexOf('function ' + name + '(');
  assert(start !== -1, 'function not found in app.html: ' + name);
  let depth = 0, i = text.indexOf('{', start), inStr = null, lc = false, bc = false;
  for (; i < text.length; i++) {
    const c = text[i], prev = text[i - 1];
    if (lc) { if (c === '\n') lc = false; continue; }
    if (bc) { if (c === '/' && prev === '*') bc = false; continue; }
    if (inStr) { if (c === inStr && prev !== '\\') inStr = null; continue; }
    if (c === '/' && text[i + 1] === '/') { lc = true; continue; }
    if (c === '/' && text[i + 1] === '*') { bc = true; continue; }
    if (c === '"' || c === "'") { inStr = c; continue; }
    if (c === '{') depth++;
    if (c === '}' && --depth === 0) { i++; break; }
  }
  return text.slice(start, i);
}

const ctx = {};
for (const name of ['ACTIVITY_FULL', 'ACTIVITY_COUNT_CAP', 'ACTIVITY_LABEL_FROM_MS', 'ACTIVITY_LABEL_TO',
                    'ACTIVITY_QUIET_MAX', 'CLOCK_CUE_MIN_SIGNALS', 'CLOCK_CUE_SLOTS']) {
  const m = new RegExp('\\b' + name + ' = ([0-9.]+)').exec(src);
  assert(m, name + ' not found in app.html');
  ctx[name] = Number(m[1]);
}
vm.createContext(ctx);
vm.runInContext(['noteActivity', 'activityBars', 'activityCount', 'activityLabel', 'clockCue', 'tallySlot', 'noteBandEvent'].map((n) => extractFunction(src, n)).join('\n'), ctx);

const P = 15000;
const SI = 119267489;
const t0 = SI * P;

let act = { si: 0, bm: 0, lv: [] };
assert.strictEqual(ctx.activityBars(act, t0 + 5000, P).length, 0, 'nothing before any event');

act = ctx.noteActivity(act, { d: 8, si: SI, bm: 160, b: 0, v: [0, 1, ctx.ACTIVITY_FULL] });
act = ctx.noteActivity(act, { d: 8, si: SI, bm: 160, b: 3, v: [ctx.ACTIVITY_FULL * 4] });
let bars = ctx.activityBars(act, t0 + 5000, P);
assert.strictEqual(bars.length, 4, 'one bar per block heard');
assert.strictEqual(bars[0].h, 0, 'level 0 is no height');
assert.strictEqual(bars[1].h, 1 / ctx.ACTIVITY_FULL, 'height is linear in the level');
assert.strictEqual(bars[2].h, 1, 'ACTIVITY_FULL fills the height');
assert.strictEqual(bars[3].h, 1, 'past ACTIVITY_FULL still fills, no more');
assert.strictEqual(bars[3].x0, 3 * 160 / P, 'block 3 starts 480 ms into the slot');
assert.strictEqual(bars[3].x1, 4 * 160 / P, 'and spans one block');

assert.strictEqual(ctx.activityBars(act, t0 + P + 100, P).length, 0,
  'the next slot shows nothing until its own events arrive');

const later = ctx.noteActivity(act, { d: 8, si: SI + 1, bm: 160, b: 2, v: [5] });
bars = ctx.activityBars(later, t0 + P + 1000, P);
assert.strictEqual(bars.length, 1, 'a later slot starts over');
assert.strictEqual(bars[0].x0, 2 * 160 / P, 'at its own block');

const stale = ctx.noteActivity(later, { d: 8, si: SI, bm: 160, b: 10, v: [9] });
assert.strictEqual(stale.si, SI + 1, 'an earlier slot is dropped');
assert.strictEqual(ctx.activityBars(stale, t0 + P + 1000, P).length, 1, 'and adds no bar');

// The label: the median of the blocks inside the signals' span.
const lab = (lv, si) => ctx.activityLabel({ si: si || SI, bm: 160, lv: lv }, t0 + 9000, P);
assert.strictEqual(lab([]), '', 'no label before any block');
assert.strictEqual(lab([0, 0, 0, 0, 20]), '', 'no label from blocks before 0.8 s');
const lv = [];
lv[5] = 12; lv[6] = 14; lv[7] = 13; lv[8] = 40; lv[9] = 0;
assert.strictEqual(lab(lv), '≈13 signals', 'the median, so one burst or gap does not move it');
const quiet = []; quiet[10] = 1; quiet[11] = 2; quiet[12] = 0;
assert.strictEqual(lab(quiet), 'Quiet', "the radio's own noise (1 or 2) reads Quiet");
const three = []; three[10] = 3;
assert.strictEqual(lab(three), '≈3 signals', 'three is a count');
const busy = []; busy[10] = 31; busy[11] = 35; busy[12] = 30;
assert.strictEqual(lab(busy), '≈' + ctx.ACTIVITY_COUNT_CAP + '+ signals', 'past the cap reads N+');
const tail = []; tail[90] = 25;
assert.strictEqual(lab(tail), '', 'blocks after the signals end are not read');
assert.strictEqual(lab(lv, SI - 1), '', 'no label for another slot');

// FT4: 7.5 s slots of 48 ms blocks.
const P4 = 7500, SI4 = 2 * SI;
const ft4 = ctx.noteActivity({ si: 0, bm: 0, lv: [] }, { d: 8, si: SI4, bm: 48, b: 20, v: [3, 4] });
bars = ctx.activityBars(ft4, SI4 * P4 + 1500, P4);
assert.strictEqual(bars.length, 2, 'FT4 blocks land');
assert.strictEqual(bars[0].x0, 20 * 48 / P4, 'at FT4 block width');

// The clock cue: signals heard but almost nothing decoded, two received slots running.
const T = (sig, dec, paused) => ({ si: 1, sig: sig, dec: dec, paused: !!paused });
assert.strictEqual(ctx.clockCue([]), '', 'no cue before any slot');
assert.strictEqual(ctx.clockCue([T(20, 1)]), '', 'one slot is not enough');
assert.strictEqual(ctx.clockCue([T(21, 0), T(20, 1)]), 'Clock? 1 decoded of ≈20 signals',
  'two slots of ~20 signals and 0-2 decodes: the 2026-10-27 clock 1.6 s off');
assert.strictEqual(ctx.clockCue([T(20, 1), T(26, 25)]), '', 'a slot that decodes clears it');
assert.strictEqual(ctx.clockCue([T(10, 6), T(8, 6)]), '', 'a real quiet band (40 m) is no cue');
assert.strictEqual(ctx.clockCue([T(2, 0), T(1, 0)]), '', 'no antenna: nothing heard, so no cue');
assert.strictEqual(ctx.clockCue([T(7, 0), T(7, 0)]), '', 'under CLOCK_CUE_MIN_SIGNALS, no cue');
assert.strictEqual(ctx.clockCue([T(20, 0), T(20, 5)]), 'Clock? 5 decoded of ≈20 signals', 'a quarter is still a cue');
assert.strictEqual(ctx.clockCue([T(20, 0), T(20, 6)]), '', 'more than a quarter is not');
assert.strictEqual(ctx.clockCue([T(20, 0), T(20, 0, true)]), '', 'a slot not decoded (PAUSED) is no cue');

// Tallying: a slot is closed with its decodes; one we sent in (no count) is left out.
const heard = []; for (let k = 5; k < 60; k++) heard[k] = 20;
let tallies = ctx.tallySlot([], { si: SI, bm: 160, lv: heard }, P, { [SI]: 1 }, {});
assert.strictEqual(tallies.length, 1, 'a received slot is tallied');
assert.strictEqual(tallies[0].sig, 20, 'with its count');
assert.strictEqual(tallies[0].dec, 1, 'and its decodes');
const txTail = []; for (let k = 82; k < 94; k++) txTail[k] = 2;
tallies = ctx.tallySlot(tallies, { si: SI + 1, bm: 160, lv: txTail }, P, {}, {});
assert.strictEqual(tallies.length, 1, 'a slot we sent in is not a received slot');
tallies = ctx.tallySlot(tallies, { si: SI + 2, bm: 160, lv: heard }, P, {}, {});
assert.strictEqual(ctx.clockCue(tallies), 'Clock? 0 decoded of ≈20 signals', 'the TX slot between does not break the run');
for (let i = 0; i < 6; i++) tallies = ctx.tallySlot(tallies, { si: SI + 3 + i, bm: 160, lv: heard }, P, {}, {});
assert.strictEqual(tallies.length, 4, 'only the newest few are kept');

// Replay: the 2026-10-27 20 m and 40 m session, trimmed to slots, decode counts and levels
// (fixtures/clock_off_2026-10-27.jsonl). After a reflash the clock sat about 1.6 s off for ten
// slots (about 20 signals, 0-2 decodes) until a QMX power cycle; then busy 20 m, 40 m, the
// antenna pulled for five slots and put back. The cue is on for exactly the bad-clock slots.
const lines = fs.readFileSync(path.join(__dirname, 'fixtures', 'clock_off_2026-10-27.jsonl'), 'utf8')
  .split('\n').filter(Boolean).map((l) => JSON.parse(l));
let st = { act: { si: 0, bm: 0, lv: [] }, tallies: [], decodes: {}, paused: {} };
const cued = [];
let lastSi = 0;
for (const e of lines) {
  st = ctx.noteBandEvent(st, e, P);
  const t = st.tallies[st.tallies.length - 1];
  if (t && t.si !== lastSi) {
    lastSi = t.si;
    if (ctx.clockCue(st.tallies)) cued.push(t.si % 10000);
  }
}
assert.deepStrictEqual(cued, [699, 700, 701, 702, 703, 704, 705, 706, 707, 708],
  'the cue shows for the ten bad-clock slots and no others');

console.log('band activity OK');
