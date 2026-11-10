#!/usr/bin/env node
// The radio bar's state follows a pushed RADIO_STATE event (AI2;) the way it follows a poll: runs
// app.html's own radioChipState(), radioReadBack() and applyRadioPush(), so a knob turn read from
// an event drifts the state and a return to the wanted state clears it.
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

const ctx = { usbState: null, everCatUp: false };
vm.createContext(ctx);
vm.runInContext(['yn', 'radioReadBack', 'radioChipState', 'applyRadioPush']
  .map((n) => extractFunction(src, n)).join('\n'), ctx);

const cfg = { cat_ready: '1', audio_rx: '1', want_hz: '14074000', radio_hz: '14074000',
              radio_tx: '0', radio_mode: '6', radio_split: '0' };
assert.strictEqual(ctx.radioChipState(cfg), 'ready', 'where the station put it');

ctx.applyRadioPush(cfg, { hz: 14074300, tx: 0, md: 6, sp: 0 });
assert.strictEqual(cfg.radio_hz, '14074300');
assert.strictEqual(ctx.radioChipState(cfg), 'drift', 'a knob turn read from a push');

ctx.applyRadioPush(cfg, { hz: 14074000, tx: 0, md: 3, sp: 0 });
assert.strictEqual(ctx.radioChipState(cfg), 'drift', 'a mode change');

ctx.applyRadioPush(cfg, { hz: 14074000, tx: 0, md: 6, sp: 1 });
assert.strictEqual(ctx.radioChipState(cfg), 'drift', 'split on');

ctx.applyRadioPush(cfg, { hz: 14074000, tx: 1, md: 6, sp: 0 });
assert.strictEqual(ctx.radioChipState(cfg), 'ready', 'our own TX is not the radio moving');
assert.strictEqual(ctx.radioReadBack(cfg).tx, true, 'and the panel reads it as transmitting');

ctx.applyRadioPush(cfg, { hz: 14074000, tx: 0, md: 6, sp: 0 });
assert.strictEqual(ctx.radioChipState(cfg), 'ready', 'back where it was');

// Tune: the QMX reports CW (md 3) for its SWR Tune carrier. That is ours, not a move; a frequency
// change during Tune still is, and once Tune is off the same mode is a move again.
cfg.tune = '1';
ctx.applyRadioPush(cfg, { hz: 14074000, tx: 1, md: 3, sp: 0 });
assert.strictEqual(ctx.radioChipState(cfg), 'ready', 'Tune\'s own mode is not the radio moving');
ctx.applyRadioPush(cfg, { hz: 14074300, tx: 1, md: 3, sp: 0 });
assert.strictEqual(ctx.radioChipState(cfg), 'drift', 'a knob turn during Tune still is');
cfg.tune = '0';
ctx.applyRadioPush(cfg, { hz: 14074000, tx: 0, md: 3, sp: 0 });
assert.strictEqual(ctx.radioChipState(cfg), 'drift', 'CW once Tune is off is a move');

// A transmission that retunes (row xvfn): the radio leaves the station's dial for FT4's and comes back. Each
// push carries the wanted dial of its own instant, so the return is not read against the poll's older one
// (webapp.md, The radio bar). Field-found 2026-11-09: without it the bar read Radio moved for up to a poll.
cfg.want_hz = '7074000';
cfg.radio_hz = '7074000';
ctx.applyRadioPush(cfg, { hz: 7047500, tx: 1, md: 6, sp: 0, want: 7047500 });
assert.strictEqual(ctx.radioChipState(cfg), 'ready', 'a retuned transmission, with its own wanted dial, reads ready');
ctx.applyRadioPush(cfg, { hz: 7074000, tx: 0, md: 6, sp: 0, want: 7074000 });
assert.strictEqual(ctx.radioChipState(cfg), 'ready', 'a retune\'s return, with its own wanted dial, reads ready');
ctx.applyRadioPush(cfg, { hz: 7075000, tx: 0, md: 6, sp: 0, want: 7074000 });
assert.strictEqual(ctx.radioChipState(cfg), 'drift', 'a knob turn is still Radio moved');
console.log('radio push: all checks passed');
