#!/usr/bin/env node
// The decode mode filter (docs/architecture/webapp.md, FT4 beside FT8 is its own batch): runs the real functions
// from app.html, as test_decode_view.js does. The chip shows only with FT4 about, cycles both, FT8, FT4, and hides
// a whole batch of the other mode.
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


const sandbox = { lastCfg: {}, decodeSlots: [], decodeView: { mode: 'all' }, DV_MODES: ['all', 'FT8', 'FT4'] };
vm.createContext(sandbox);
vm.runInContext(['slotMode', 'decodeModeChipOn', 'decodeSlotModeShown']
  .map((n) => extractFunction(src, n)).join('\n'), sandbox);

const ft8 = Object.assign([{ id: 1, x: 'CQ K1ABC FN42' }], { lenMs: 15000 });
const ft4 = Object.assign([{ id: 2, x: 'CQ K2DEF FN31' }], { lenMs: 7500 });

// No FT4 anywhere: the chip is hidden and filters nothing, whatever mode was saved.
sandbox.decodeSlots = [ft8];
sandbox.decodeView.mode = 'FT4';
assert.strictEqual(sandbox.decodeModeChipOn(), false, 'no FT4: no chip');
assert.strictEqual(sandbox.decodeSlotModeShown(ft8), true, 'a hidden chip hides nothing');
// rx_ft4 on, or an FT4 batch listed: the chip shows and the mode filters whole batches.
sandbox.lastCfg = { rx_ft4: '1' };
assert.strictEqual(sandbox.decodeModeChipOn(), true, 'rx_ft4 on: the chip');
sandbox.lastCfg = {};
sandbox.decodeSlots = [ft8, ft4];
assert.strictEqual(sandbox.decodeModeChipOn(), true, 'an FT4 batch listed: the chip');
sandbox.decodeView.mode = 'all';
assert.deepStrictEqual([ft8, ft4].map(sandbox.decodeSlotModeShown), [true, true], 'both');
sandbox.decodeView.mode = 'FT8';
assert.deepStrictEqual([ft8, ft4].map(sandbox.decodeSlotModeShown), [true, false], 'FT8 only');
sandbox.decodeView.mode = 'FT4';
assert.deepStrictEqual([ft8, ft4].map(sandbox.decodeSlotModeShown), [false, true], 'FT4 only');
console.log('PASS: decode mode filter');
