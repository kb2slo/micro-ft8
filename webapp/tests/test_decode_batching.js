#!/usr/bin/env node
// Regression test for the 2026-09-26 decode-duplicate bug: a poll response
// spanning more than one real FT8 slot (a lagging or backgrounded tab
// catching up several /api/events ring entries at once) rendered as one
// merged Decodes batch, which read as the same station's CQ appearing twice
// with different SNR/DT.
//
// Rather than re-implementing app.html's batching logic here -- a second
// copy that could silently drift from the real thing -- this pulls the exact
// function/var source out of app.html and runs it for real. If a future edit
// changes the batching behaviour, this test exercises the actual code, not a
// stale description of it.
//
// No dependencies beyond Node's own `vm`: this repo ships no build step for
// the webapp, and this test doesn't need one either.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const APP_HTML = path.join(__dirname, '..', 'app', 'app.html');
const src = fs.readFileSync(APP_HTML, 'utf8');

// Extracts `function NAME(...) { ... }` by counting braces, skipping over
// string literals and comments so a `{`/`}` in a comment or string can't
// throw off the match (this file's comments are prose-heavy).
function extractFunction(text, name) {
  const marker = 'function ' + name + '(';
  const start = text.indexOf(marker);
  assert(start !== -1, 'function not found in app.html: ' + name);
  const braceStart = text.indexOf('{', start);
  assert(braceStart !== -1, 'no body found for: ' + name);
  let depth = 0;
  let inStr = null, inLineComment = false, inBlockComment = false;
  let i = braceStart;
  for (; i < text.length; i++) {
    const c = text[i];
    const prev = text[i - 1];
    if (inLineComment) { if (c === '\n') inLineComment = false; continue; }
    if (inBlockComment) { if (c === '/' && prev === '*') inBlockComment = false; continue; }
    if (inStr) { if (c === inStr && prev !== '\\') inStr = null; continue; }
    if (c === '/' && text[i + 1] === '/') { inLineComment = true; continue; }
    if (c === '/' && text[i + 1] === '*') { inBlockComment = true; continue; }
    if (c === '"' || c === "'") { inStr = c; continue; }
    if (c === '{') { depth++; continue; }
    if (c === '}') {
      depth--;
      if (depth === 0) { i++; break; }
    }
  }
  assert(depth === 0, 'unbalanced braces extracting: ' + name);
  return text.slice(start, i);
}

// Extracts a single-line `var NAME = ...;` declaration verbatim (e.g. the
// DECODE_SLOTS_MAX cap), so the test tracks the real constant instead of a
// second hardcoded copy.
function extractVarLine(text, name) {
  const re = new RegExp('var\\s+' + name + '\\b\\s*=[^\\n]*;');
  const m = re.exec(text);
  assert(m, 'var declaration not found in app.html: ' + name);
  return m[0];
}

const extracted = [
  extractVarLine(src, 'pendingDecodes'),
  extractVarLine(src, 'pendingDecodesSp'),
  extractVarLine(src, 'pendingDecodesT'),
  extractVarLine(src, 'freqHistory'),
  extractVarLine(src, 'slotState'),
  extractVarLine(src, 'decodeSlots'),
  extractVarLine(src, 'DECODE_SLOTS_MAX'),
  extractFunction(src, 'decodeSortGroup'),
  extractFunction(src, 'decodeSortCmp'),
  extractFunction(src, 'freqHeardDuring'),
  extractFunction(src, 'flushDecodeBatch'),
  extractFunction(src, 'applyEvent'),
].join('\n\n');

function freshSandbox() {
  const sandbox = {
    console: console,
    // Deliberate stubs for state/functions applyEvent touches that are
    // unrelated to batching correctness: the live config cache (only
    // band/freq are read), the rolling decode-rate counter, and the
    // opportunistic callsign->grid cache.
    lastCfg: { band_name: '15m', freq_khz: '21074' },
    recordDecodeBatch: function () {},
    learnCallsignGrid: function () {},
  };
  vm.createContext(sandbox);
  vm.runInContext(extracted, sandbox, { filename: 'app.html (extracted)' });
  return sandbox;
}

function decodeEvt(id, dt, hz, snr, x, extra) {
  return Object.assign({ d: 1, id: id, dt: dt, hz: hz, snr: snr, x: x, cq: true }, extra || {});
}
function slotStateEvt(sp) {
  return { d: 3, sp: sp, bm: 0, hz: 0 };
}

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log('PASS: ' + name);
  } catch (e) {
    failures++;
    console.log('FAIL: ' + name);
    console.log('  ' + (e && e.message ? e.message : e));
  }
}

// ---- The reported bug: two real slots land in one poll response ----------
check('two real slots in one poll render as two separate batches, not one', () => {
  const sb = freshSandbox();
  const events = [
    slotStateEvt(0),
    decodeEvt(1, -0.2, 844, 34, 'CQ TI5RR EK70'),
    slotStateEvt(1), // real slot boundary: parity flips
    decodeEvt(2, 1.5, 562, 12, 'CQ W6ACT CM97'),
    decodeEvt(3, 0.0, 844, 5, 'CQ TI5RR EK70'),
  ];
  events.forEach(sb.applyEvent);
  sb.flushDecodeBatch(); // pollEvents() always flushes whatever remains at the end of a poll

  assert.strictEqual(sb.decodeSlots.length, 2, 'expected one batch per real slot');
  // decodeSlots is newest-first (unshift).
  const older = sb.decodeSlots[1];
  const newer = sb.decodeSlots[0];
  assert.strictEqual(older.length, 1);
  assert.strictEqual(older[0].snr, 34);
  assert.strictEqual(newer.length, 2);
  // .join(), not deepStrictEqual on the arrays themselves: they're built by
  // code running in a separate vm realm, so their Array constructor differs
  // from this file's even when every element matches.
  assert.strictEqual(newer.map((d) => d.snr).sort((a, b) => a - b).join(','), '5,12');
});

// ---- Normal cadence: one slot's decodes in one poll stay one batch --------
check('decodes from a single real slot still render as one batch', () => {
  const sb = freshSandbox();
  const events = [
    slotStateEvt(0),
    decodeEvt(1, -0.2, 844, 34, 'CQ TI5RR EK70'),
    decodeEvt(2, 1.5, 562, 12, 'CQ W6ACT CM97'),
  ];
  events.forEach(sb.applyEvent);
  sb.flushDecodeBatch();

  assert.strictEqual(sb.decodeSlots.length, 1);
  assert.strictEqual(sb.decodeSlots[0].length, 2);
});

// ---- No SLOT_STATE observed yet (fresh session): must not throw ----------
check('decodes before any SLOT_STATE is seen do not crash or wrongly split', () => {
  const sb = freshSandbox();
  const events = [
    decodeEvt(1, -0.2, 844, 34, 'CQ TI5RR EK70'),
    decodeEvt(2, 1.5, 562, 12, 'CQ W6ACT CM97'),
  ];
  events.forEach(sb.applyEvent);
  sb.flushDecodeBatch();

  assert.strictEqual(sb.decodeSlots.length, 1);
  assert.strictEqual(sb.decodeSlots[0].length, 2);
});

// ---- DECODE_SLOTS_MAX cap still holds across the extra flush point -------
check('decodeSlots stays capped at DECODE_SLOTS_MAX after a multi-slot poll', () => {
  const sb = freshSandbox();
  const events = [];
  for (let slot = 0; slot < sb.DECODE_SLOTS_MAX + 2; slot++) {
    events.push(slotStateEvt(slot % 2));
    events.push(decodeEvt(slot, 0, 800, 10, 'CQ N0CALL AA00'));
  }
  events.forEach(sb.applyEvent);
  sb.flushDecodeBatch();

  assert.strictEqual(sb.decodeSlots.length, sb.DECODE_SLOTS_MAX);
});

// ---- a slot is labelled with the frequency it was heard on ----------------
// Decodes land a second or two after their slot ends. A band change in that
// gap must not relabel them with the new band (bench, 2026-09-28: Reply then
// called a station on a band it was not heard on).
function slotBoundaries() {
  const endSec = Math.floor(Date.now() / 1000 / 15) * 15; // the slot that just ended ends here
  return { startMs: (endSec - 15) * 1000, endMs: endSec * 1000, madeAt: endSec + 1 };
}

check('decodes arriving after a band change keep the band their slot was heard on', () => {
  const sb = freshSandbox();
  const b = slotBoundaries();
  sb.freqHistory.push({ ms: 0, khz: 7074, band: '40m' });
  sb.freqHistory.push({ ms: b.endMs + 500, khz: 14074, band: '20m' }); // changed after the slot ended
  sb.lastCfg = { band_name: '20m', freq_khz: '14074' };
  sb.applyEvent(decodeEvt(1, 0.1, 900, -5, 'CQ K1ABC FN42', { t: b.madeAt }));
  sb.flushDecodeBatch();
  assert.strictEqual(sb.decodeSlots[0].freqKhz, '7074');
  assert.strictEqual(sb.decodeSlots[0].band, '40m');
  assert.strictEqual(sb.decodeSlots[0].freqChanged, false);
});

check('a slot the radio was retuned during is marked, with no frequency', () => {
  const sb = freshSandbox();
  const b = slotBoundaries();
  sb.freqHistory.push({ ms: 0, khz: 7074, band: '40m' });
  sb.freqHistory.push({ ms: b.startMs + 5000, khz: 14074, band: '20m' }); // changed mid-slot
  sb.applyEvent(decodeEvt(1, 0.1, 900, -5, 'CQ K1ABC FN42', { t: b.madeAt }));
  sb.flushDecodeBatch();
  assert.strictEqual(sb.decodeSlots[0].freqChanged, true);
  assert.strictEqual(sb.decodeSlots[0].freqKhz, '');
});

check('with no frequency history, a slot takes the latest config (as before)', () => {
  const sb = freshSandbox();
  sb.applyEvent(decodeEvt(1, 0.1, 900, -5, 'CQ K1ABC FN42'));
  sb.flushDecodeBatch();
  assert.strictEqual(sb.decodeSlots[0].freqKhz, '21074');
  assert.strictEqual(sb.decodeSlots[0].band, '15m');
});

if (failures > 0) {
  console.log(failures + ' check(s) failed');
  process.exit(1);
}
console.log('All decode-batching checks passed');
