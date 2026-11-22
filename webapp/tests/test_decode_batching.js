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
  extractVarLine(src, 'pendingBatches'),
  extractFunction(src, 'newPendingBatch'),
  extractFunction(src, 'flushDecodeBatches'),
  extractVarLine(src, 'slotState'),
  extractVarLine(src, 'decodeSlots'),
  extractVarLine(src, 'DECODE_SLOTS_MAX'),
  extractVarLine(src, 'queue'),
  extractVarLine(src, 'queueOrder'),
  extractVarLine(src, 'txHud'),
  extractVarLine(src, 'QUEUE_STATE_IDLE'),
  extractFunction(src, 'isReplyableDecode'),
  extractFunction(src, 'decodeSortGroup'),
  extractFunction(src, 'decodeSortCmp'),
  extractFunction(src, 'flushDecodeBatch'),
  extractFunction(src, 'fmtSlotDateTime'),
  extractFunction(src, 'fmtSlotSpan'),
  extractFunction(src, 'slotReplyOpen'),
  extractFunction(src, 'resetLiveState'),
  extractFunction(src, 'applyEvent'),
].join('\n\n');

function freshSandbox() {
  const sandbox = {
    console: console,
    // Deliberate stubs for state/functions applyEvent touches that are
    // unrelated to batching correctness: the live config cache (only
    // band/freq are read), the rolling decode-rate counter, and the
    // opportunistic callsign->grid cache, and the slot bar's band tally.
    lastCfg: { band_name: '15m', freq_khz: '21074' },
    recordDecodeBatch: function () {},
    learnCallsignGrid: function () {},
    band: {},
    noteBandEvent: function () {},
    SLOT_MS: 15000,
    // Counts the times a slot event said the main board's clock is unset (time.md).
    clockUnsets: 0,
    clockUnsetSeen: function () { sandbox.clockUnsets++; },
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

// ---- FT4 beside FT8: an FT4 decode between one FT8 slot's decodes (bench, 2026-11-16) ----
// One open batch for both modes split the FT8 slot in two around it: 19:08:15-30z showed twice.
check('an FT4 decode arriving between one FT8 slot\'s decodes does not split that slot', () => {
  const sb = freshSandbox();
  const ft8 = (id, x) => decodeEvt(id, 0.1, 1000 + id, -5, x, { si: 119657077, sl: 150, fq: 14074000 });
  const ft4 = (id, x) => decodeEvt(id, 0.1, 1000 + id, -5, x, { si: 239314154, sl: 75, fq: 14080000 });
  [ft8(1, 'CQ A1AA FN20'), ft8(2, 'CQ B1BB FN20'), ft4(3, 'CQ C1CC FN20'), ft8(4, 'CQ D1DD FN20')].forEach(sb.applyEvent);
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots.length, 2, 'one FT8 batch and one FT4 batch');
  const ft8Slot = sb.decodeSlots.find((s) => s.lenMs === 15000);
  const ft4Slot = sb.decodeSlots.find((s) => s.lenMs === 7500);
  assert.strictEqual(ft8Slot.length, 3, 'the FT8 slot keeps all three');
  assert.strictEqual(ft4Slot.length, 1);
  // The FT8 slot ends at 19:08:30 and the FT4 one at 19:08:22.5: the later is on top.
  assert.strictEqual(sb.decodeSlots[0], ft8Slot, 'the slot that ends later is first');
});

check('each mode still starts a new batch on its own next slot', () => {
  const sb = freshSandbox();
  const ft8 = (id, si) => decodeEvt(id, 0.1, 1000 + id, -5, 'CQ A1AA FN20', { si: si, sl: 150, fq: 14074000 });
  const ft4 = (id, si) => decodeEvt(id, 0.1, 1000 + id, -5, 'CQ C1CC FN20', { si: si, sl: 75, fq: 14080000 });
  // FT4's next slot closes its first, with FT8's slot still open across both (DECODE_SLOTS_MAX is 3).
  [ft4(1, 200), ft8(2, 100), ft4(3, 201), ft8(4, 100)].forEach(sb.applyEvent);
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots.length, 3, 'one FT8 slot and two FT4 slots');
  assert.strictEqual(sb.decodeSlots.filter((s) => s.lenMs === 7500).length, 2, 'FT4 split on its own next slot');
  const ft8Slot = sb.decodeSlots.find((s) => s.lenMs === 15000);
  assert.strictEqual(ft8Slot.length, 2, 'FT8 kept both of its slot\'s decodes across FT4\'s');
});

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
  sb.flushDecodeBatches(); // pollEvents() always flushes whatever remains at the end of a poll

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
  sb.flushDecodeBatches();

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
  sb.flushDecodeBatches();

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
  sb.flushDecodeBatches();

  assert.strictEqual(sb.decodeSlots.length, sb.DECODE_SLOTS_MAX);
});

// ---- Port A v2: the host names each decode's slot and dial frequency -----
// Decodes land a second or two after their slot ends, so the app cannot tell
// their slot or frequency from when they arrive (bench, 2026-09-28: a band
// change in that gap relabelled a slot, and Reply called a station on a band
// it was not heard on). A v2 host sends both.
const SLOT = 119267489; // a 2026 FT8 slot index; it ends at (SLOT + 1) * 15 s

check('a v2 slot is timed at its end, on the 15 s grid', () => {
  const sb = freshSandbox();
  sb.applyEvent(decodeEvt(1, 0.1, 900, -5, 'CQ K1ABC FN42', { si: SLOT, sl: 150, fq: 7074000 }));
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots[0].ts, (SLOT + 1) * 15000);
  assert.strictEqual((sb.decodeSlots[0].ts / 1000) % 15, 0);
  assert.strictEqual(sb.decodeSlots[0].freqKhz, '7074');
});

check('an FT4 slot ends on its 7.5 s grid', () => {
  const sb = freshSandbox();
  sb.applyEvent(decodeEvt(1, 0.1, 900, -5, 'CQ K1ABC FN42', { si: 2 * SLOT + 1, sl: 75, fq: 7047500 }));
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots[0].ts, (2 * SLOT + 2) * 7500);
  assert.strictEqual(sb.decodeSlots[0].freqKhz, '7047.5');
});

check('the host\'s frequency labels a slot, not the config current when it arrives', () => {
  const sb = freshSandbox(); // lastCfg says 15 m / 21074: the radio has moved on since
  sb.applyEvent(decodeEvt(1, 0.1, 900, -5, 'CQ K1ABC FN42', { si: SLOT, sl: 150, fq: 7074000 }));
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots[0].freqKhz, '7074');
  assert.strictEqual(sb.decodeSlots[0].band, ''); // named from freqKhz when drawn
});

check('a slot retuned partway through becomes two batches with the same time, and keeps every decode', () => {
  const sb = freshSandbox();
  // Sorted by SNR on the host, so the two frequencies interleave.
  sb.applyEvent(decodeEvt(1, 0.1, 900, -3, 'CQ K1ABC FN42', { si: SLOT, sl: 150, fq: 14074000 }));
  sb.applyEvent(decodeEvt(2, 0.2, 1200, -8, 'CQ W2DEF FN20', { si: SLOT, sl: 150, fq: 7074000 }));
  sb.applyEvent(decodeEvt(3, 0.3, 1500, -12, 'CQ N3GHI FN10', { si: SLOT, sl: 150, fq: 14074000 }));
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots.length, 2);
  const byFreq = {};
  sb.decodeSlots.forEach(slot => { byFreq[slot.freqKhz] = slot; });
  assert.strictEqual(byFreq['14074'].length, 2);
  assert.strictEqual(byFreq['7074'].length, 1);
  assert.strictEqual(byFreq['14074'].ts, byFreq['7074'].ts);
});

check('a decode with no frequency joins its slot\'s named frequency instead of forming a second batch', () => {
  const sb = freshSandbox();
  sb.applyEvent(decodeEvt(1, 0.1, 900, -3, 'CQ K1ABC FN42', { si: SLOT, sl: 150, fq: 0 }));
  sb.applyEvent(decodeEvt(2, 0.2, 1200, -8, 'CQ W2DEF FN20', { si: SLOT, sl: 150, fq: 14074000 }));
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots.length, 1);
  assert.strictEqual(sb.decodeSlots[0].length, 2);
  assert.strictEqual(sb.decodeSlots[0].freqKhz, '14074');
});

check('the next v2 slot starts a new batch without any SLOT_STATE parity flip', () => {
  const sb = freshSandbox();
  sb.applyEvent(decodeEvt(1, 0.1, 900, -5, 'CQ K1ABC FN42', { si: SLOT, sl: 150, fq: 7074000 }));
  sb.applyEvent(decodeEvt(2, 0.1, 900, -5, 'CQ K1ABC FN42', { si: SLOT + 1, sl: 150, fq: 7074000 }));
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots.length, 2);
  assert.strictEqual(sb.decodeSlots[0].ts - sb.decodeSlots[1].ts, 15000);
});

check('without slot fields (a v1 host), a batch takes the latest config and arrival time', () => {
  const sb = freshSandbox();
  const before = Date.now();
  sb.applyEvent(decodeEvt(1, 0.1, 900, -5, 'CQ K1ABC FN42'));
  sb.flushDecodeBatches();
  assert.strictEqual(sb.decodeSlots[0].freqKhz, '21074');
  assert.strictEqual(sb.decodeSlots[0].band, '15m');
  assert(sb.decodeSlots[0].ts >= before);
});

// ---- the slot's span in its header, and the reply window ------------------
const at = (h, m, sec, ms) => Date.UTC(2026, 8, 28, h, m, sec, ms || 0);

check('a v2 header shows the slot span, dropping what the end shares with the start', () => {
  const sb = freshSandbox();
  assert.strictEqual(sb.fmtSlotSpan({ ts: at(19, 3, 45), lenMs: 15000 }), '2026-09-28 19:03:30–45z');
  assert.strictEqual(sb.fmtSlotSpan({ ts: at(19, 4, 0), lenMs: 15000 }), '2026-09-28 19:03:45–04:00z');
  assert.strictEqual(sb.fmtSlotSpan({ ts: at(20, 0, 0), lenMs: 15000 }), '2026-09-28 19:59:45–20:00:00z');
  assert.strictEqual(sb.fmtSlotSpan({ ts: at(19, 3, 15), lenMs: 7500 }), '2026-09-28 19:03:07.5–15z');
  assert.strictEqual(sb.fmtSlotSpan({ ts: at(19, 3, 22, 500), lenMs: 7500 }), '2026-09-28 19:03:15–22.5z');
});

check('without a slot length (a v1 host), the header shows the one time it has', () => {
  const sb = freshSandbox();
  assert.strictEqual(sb.fmtSlotSpan({ ts: at(19, 3, 47), lenMs: 0 }), '2026-09-28 19:03:47z');
});

check('Reply stays open until two slot lengths after a slot ends, by the clock', () => {
  const sb = freshSandbox();
  const end = at(19, 3, 45);
  assert.strictEqual(sb.slotReplyOpen({ ts: end, lenMs: 15000 }, end + 2000), true);   // just arrived
  assert.strictEqual(sb.slotReplyOpen({ ts: end, lenMs: 15000 }, end + 29999), true);  // one slot on
  assert.strictEqual(sb.slotReplyOpen({ ts: end, lenMs: 15000 }, end + 30000), false); // two slots on
  assert.strictEqual(sb.slotReplyOpen({ ts: end, lenMs: 7500 }, end + 14999), true);   // FT4: 15 s
  assert.strictEqual(sb.slotReplyOpen({ ts: end, lenMs: 7500 }, end + 15000), false);
});

check('coming back after a break, the newest slot shown is not replyable if its time has passed', () => {
  const sb = freshSandbox();
  const now = at(19, 10, 3);
  // The list's newest slot, restored from the cache or caught up from the
  // sidekick, ended five minutes ago: first in the list, but closed.
  assert.strictEqual(sb.slotReplyOpen({ ts: at(19, 5, 0), lenMs: 15000 }, now), false);
  // A slot that ended three seconds ago is open, wherever it sits.
  assert.strictEqual(sb.slotReplyOpen({ ts: at(19, 10, 0), lenMs: 15000 }, now), true);
});

// ---- decodeSortGroup/decodeSortCmp: replyable outranks non-replyable ------
// (found 2026-09-30, operator report: "replyable messages should sort at
// the top, below messages to me" -- the gap was a replyable 73/RR73 not
// addressed to the operator, which fell into the plain "other" group.)

function decodeObj(over) {
  return Object.assign({ me: false, cq: false, rq: false, x: 'K1ABC W1AW -09', snr: 0 }, over || {});
}

check('a message to me sorts first, even with nothing to reply to', () => {
  const sb = freshSandbox();
  const toMe = decodeObj({ me: true, x: 'W1AW K1ABC -09' }); // a plain report, no Reply button
  const cq = decodeObj({ cq: true, x: 'CQ K1ABC FN42' });
  const sorted = [cq, toMe].sort(sb.decodeSortCmp);
  assert.strictEqual(sorted[0], toMe, 'to_me outranks a replyable CQ');
});

check('a CQ outranks a plain, non-replyable line', () => {
  const sb = freshSandbox();
  const cq = decodeObj({ cq: true, x: 'CQ K1ABC FN42' });
  const other = decodeObj({ x: 'K1ABC W1AW -09' }); // a bystander's report, not replyable
  const sorted = [other, cq].sort(sb.decodeSortCmp);
  assert.strictEqual(sorted[0], cq);
});

check('a replyable 73/RR73 not addressed to me now outranks a plain line (the gap this closes)', () => {
  const sb = freshSandbox();
  const closing = decodeObj({ x: 'K1ABC W1AW 73' });  // someone else's sign-off -- replyable
  const other = decodeObj({ x: 'K1ABC W1AW -09' });   // a plain report -- not replyable
  const sorted = [other, closing].sort(sb.decodeSortCmp);
  assert.strictEqual(sorted[0], closing, 'a replyable 73 must outrank a non-replyable line');
});

check('a replyable RR73 not addressed to me outranks a recent-QSO line', () => {
  const sb = freshSandbox();
  const closing = decodeObj({ x: 'K1ABC W1AW RR73' });
  const recent = decodeObj({ rq: true, x: 'K1ABC W1AW -09' });
  const sorted = [recent, closing].sort(sb.decodeSortCmp);
  assert.strictEqual(sorted[0], closing);
});

check('a non-replyable line addressed to me still outranks a replyable CQ', () => {
  const sb = freshSandbox();
  const toMeReport = decodeObj({ me: true, x: 'W1AW K1ABC -09' });
  const cq = decodeObj({ cq: true, x: 'CQ K1ABC FN42' });
  const sorted = [cq, toMeReport].sort(sb.decodeSortCmp);
  assert.strictEqual(sorted[0], toMeReport, 'to_me wins outright, even over something replyable');
});

check('within the replyable tier, CQs still rank by strongest SNR first', () => {
  const sb = freshSandbox();
  const weak = decodeObj({ cq: true, x: 'CQ K1ABC FN42', snr: -10 });
  const strong = decodeObj({ cq: true, x: 'CQ N0CALL EM10', snr: 5 });
  const sorted = [weak, strong].sort(sb.decodeSortCmp);
  assert.strictEqual(sorted[0], strong);
});

check('a CQ from a recently-worked station now sorts with the replyable lines, not last', () => {
  const sb = freshSandbox();
  // Just worked K1ABC; they call CQ again -- previously fell into the
  // recent-QSO group (checked before is_cq) and sorted last.
  const cqAgain = decodeObj({ cq: true, rq: true, x: 'CQ K1ABC FN42' });
  const other = decodeObj({ x: 'N0CALL W1AW -09' });
  const sorted = [other, cqAgain].sort(sb.decodeSortCmp);
  assert.strictEqual(sorted[0], cqAgain, 'is_cq (replyable) now outranks is_recent_qso');
});

// ---- A reload replaces the slots restored from the cache with the sidekick's ----------
check("a reload's /api/state reply replaces the cached slots, queue and TX HUD", () => {
  const sb = freshSandbox();
  // What loadStateCache() restored: a slot from long ago, a finished QSO, a transmission.
  sb.applyEvent(decodeEvt(7, 0.1, 900, -5, 'CQ OLD1 FN42', { si: SLOT - 400, sl: 150, fq: 14074000 }));
  sb.flushDecodeBatches();
  sb.queue = { 9: { dxcall: 'OLD1', state: 2 } };
  sb.queueOrder = ['9'];
  sb.txHud = { active: true };

  sb.resetLiveState();
  [
    decodeEvt(1, 0.1, 900, -5, 'CQ N6HAN CM87', { si: SLOT, sl: 150, fq: 14074000 }),
    decodeEvt(2, 0.0, 1200, -12, 'AG6AQ N6HAN -12', { si: SLOT + 1, sl: 150, fq: 14074000 }),
  ].forEach(sb.applyEvent);
  sb.flushDecodeBatches();

  assert.strictEqual(sb.decodeSlots.length, 2, 'only the slots the sidekick has');
  assert.strictEqual(sb.decodeSlots.map((sl) => sl[0].x).join('|'), 'AG6AQ N6HAN -12|CQ N6HAN CM87');
  assert.strictEqual(Object.keys(sb.queue).length, 0, 'no cached queue entry survives');
  assert.strictEqual(sb.txHud, null, 'no cached transmission survives');
});

check('a slot event with no time asks for a clock set; one with a time does not', () => {
  const sb = freshSandbox();
  sb.applyEvent({ d: 3, sp: 0, hz: 0, t: 1795000000 });
  assert.strictEqual(sb.clockUnsets, 0, 'a set clock asks nothing');
  sb.applyEvent({ d: 3, sp: 1, hz: 0, t: 0 });
  assert.strictEqual(sb.clockUnsets, 1, 't 0 is the main board saying its clock is unset');
});

if (failures > 0) {
  console.log(failures + ' check(s) failed');
  process.exit(1);
}
console.log('All decode-batching checks passed');
