#!/usr/bin/env node
// Worked and activation cues on Operate (docs/architecture/webapp.md, Worked and activation cues on
// decodes): runs the real dupe, badge and progress functions from app.html. A dupe is the same call,
// band and mode in today's file, and with references, logged at one of them; without references
// every QSO in the file counts. Another band, mode or reference is not a dupe; P2P/S2S only when the
// program is ours. Progress: a park counts calls unique by band and mode, a summit different calls,
// and an n-fer record counts for each of its references.
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

const sandbox = { lastCfg: {} };
vm.createContext(sandbox);
vm.runInContext(['parseActRefs', 'refsOverlap', 'dupeKey', 'spotBadgeLabel', 'slotMode', 'activationCounts']
  .map((n) => extractFunction(src, n)).join('\n'), sandbox);

const refs = sandbox.parseActRefs({ act_refs: 'POTA:US-1234' });
// A call on a band: FT8 and FT4 count as one mode, as POTA counts every digital mode as DATA. Whether a decode is a
// dupe is the main board's (qso_log_store_worked(), host_mock/test_qso_log_store.cpp); this key is the progress line's.
assert.strictEqual(sandbox.dupeKey('n0abc', '40M'), sandbox.dupeKey('N0ABC', '40m'), 'case does not matter');
assert.notStrictEqual(sandbox.dupeKey('N0ABC', '40m'), sandbox.dupeKey('N0ABC', '20m'), 'another band is another key');

// The poll interval is assigned before the timer reads it; read while undefined it was 0, and
// Operate re-read today's log back to back (85 reads in 25 s on the bench, 2026-11-09).
const refreshDef = src.indexOf('var LOG_REFRESH_MS = ');
assert(refreshDef !== -1 && refreshDef < src.indexOf('setInterval(refreshOpLog, LOG_REFRESH_MS)'),
       'LOG_REFRESH_MS is assigned before setInterval(refreshOpLog) reads it');

const pota = { program: 'POTA', ref: 'US-77' }, sota = { program: 'SOTA', ref: 'W7O/CC-001' };
assert.strictEqual(sandbox.spotBadgeLabel(pota, refs), 'P2P US-77');
assert.strictEqual(sandbox.spotBadgeLabel(sota, refs), 'SOTA W7O/CC-001', 'a summit spot at a park activation is not S2S');
assert.strictEqual(sandbox.spotBadgeLabel(sota, sandbox.parseActRefs({ act_refs: 'SOTA:W7O/CC-002' })), 'S2S W7O/CC-001');
assert.strictEqual(sandbox.spotBadgeLabel(null, refs), '');
assert.strictEqual(sandbox.slotMode({ lenMs: 7500 }), 'FT4');
assert.strictEqual(sandbox.slotMode({ lenMs: 15000 }), 'FT8');
const progress = (rs, refsStr) => sandbox.activationCounts(rs, sandbox.parseActRefs({ act_refs: refsStr }))
  .map((c) => c.ref + ' ' + c.n + '/' + c.target).join(', ');
const log = [
  { call: 'W1AW', band: '20m', mode: 'FT8', my_pota_ref: 'US-1234', my_sota_ref: 'W2/XX-001' },
  { call: 'W1AW', band: '20m', mode: 'FT8', my_pota_ref: 'US-1234', my_sota_ref: 'W2/XX-001' },
  { call: 'W1AW', band: '40m', mode: 'FT8', my_pota_ref: 'US-1234', my_sota_ref: 'W2/XX-001' },
  { call: 'K9XYZ', band: '20m', mode: 'FT8', my_pota_ref: 'US-1234,US-5678' },
  { call: 'N0ABC', band: '20m', mode: 'FT8', my_pota_ref: 'US-9999' },
  { call: 'W1AW', band: '20m', mode: 'FT4', my_pota_ref: 'US-1234' },
];
assert.strictEqual(progress(log, 'POTA:US-1234,POTA:US-5678,SOTA:W2/XX-001'),
  'US-1234 3/10, US-5678 1/10, W2/XX-001 1/4',
  'a repeat on the same band counts once, in FT8 or FT4 alike (DATA); another band counts for a park; an n-fer counts for each; ' +
  'a summit counts the call once across bands; another reference does not count');
assert.strictEqual(progress([], 'POTA:US-1234'), 'US-1234 0/10');
assert.strictEqual(progress(log, ''), '', 'no references, no line');

console.log('PASS: activation cues');
