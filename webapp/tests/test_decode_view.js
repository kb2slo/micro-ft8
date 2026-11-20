#!/usr/bin/env node
// Decode sort and filter (docs/architecture/webapp.md, Decode sort and filter): runs the real functions from app.html, as
// test_decode_batching.js does. Within a slot; filters combine; a message to us stays on top and
// is never hidden; a decode with no grid sorts last under Nearest and Farthest; Hide worked drops
// what the Worked flag marks; the Filter pill's label names what is on, and the sheet's Apply sets
// exactly what is ticked.
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

const sandbox = {};
vm.createContext(sandbox);
vm.runInContext("var DV_FILTERS = ['reply', 'park', 'worked'];\n" +
  ['decodeIsPark', 'isReplyableDecode', 'decodePassesView', 'decodeViewOf', 'decodeFilterLabel', 'decodeViewWithFilters']
  .map((n) => extractFunction(src, n)).join('\n'), sandbox);
assert(src.includes("var DV_FILTERS = ['reply', 'park', 'worked'];"), 'DV_FILTERS changed: update this test');

const km = { A: 4000, B: 800, C: 12000, D: null, ME: 100 };
const distKmOf = (d) => km[d.call];
const slot = [
  { id: 1, call: 'ME', me: true, cq: false, snr: -20, x: 'K1ABC KB2SLO -05' },
  { id: 2, call: 'A', cq: true, snr: -10, x: 'CQ W1AW FN31' },
  { id: 3, call: 'B', cq: true, snr: 5, x: 'CQ POTA K9XYZ EN52' },
  { id: 4, call: 'C', cq: false, snr: 12, x: 'W2AAA W3BBB R-10' },
  { id: 6, call: 'A', cq: false, snr: 0, x: 'W2AAA W3BBB RR73' },
  { id: 5, call: 'D', cq: true, snr: -3, x: 'CQ SOTA N0DDD' },
];
const view = (o) => Object.assign({ sort: 'default', reply: false, park: false, worked: false }, o);
const ids = (v) => sandbox.decodeViewOf(slot, v, distKmOf).map((d) => d.id);
const eq = (got, want, msg) => assert.deepStrictEqual(got, want, msg);

eq(ids(view()), [1, 2, 3, 4, 6, 5], 'default keeps the stored order');
eq(ids(view({ sort: 'snr' })), [1, 4, 3, 6, 5, 2], 'Strongest: the message to us on top, then by SNR');
eq(ids(view({ sort: 'near' })), [1, 3, 2, 6, 4, 5], 'Nearest: no-grid last');
eq(ids(view({ sort: 'far' })), [1, 4, 2, 6, 3, 5], 'Farthest: the message to us on top, then by distance, no-grid last');
eq(ids(view({ reply: true })), [1, 2, 3, 6, 5], 'Replyable keeps a CQ, a closing 73 or RR73, and the message to us; a report is out');
eq(ids(view({ park: true })), [1, 3, 5], 'POTA/SOTA filter');
eq(ids(view({ reply: true, park: true })), [1, 3, 5], 'filters combine');
// A row about a station in our queue passes every filter and comes right after the message to us.
const involves = (d) => d.id === 4;  // W2AAA W3BBB R-10, with W3BBB queued
const idsQ = (v) => sandbox.decodeViewOf(slot, v, distKmOf, involves).map((d) => d.id);
eq(idsQ(view()), [1, 4, 2, 3, 6, 5], 'the queued station\'s row moves up under the message to us');
eq(idsQ(view({ reply: true })), [1, 4, 2, 3, 6, 5], 'Replyable does not hide it');
eq(idsQ(view({ park: true })), [1, 4, 3, 5], 'nor does POTA/SOTA');
eq(idsQ(view({ sort: 'far' })), [1, 4, 2, 6, 3, 5], 'any sort: after the message to us');
// Hide worked: the rows whose sender the Worked flag marks go, except the message to us and a
// queued station's row; off, the flag hides nothing.
const worked = (d) => d.id === 1 || d.id === 2 || d.id === 4;
const idsW = (v, inv) => sandbox.decodeViewOf(slot, v, distKmOf, inv, worked).map((d) => d.id);
eq(idsW(view()), [1, 2, 3, 4, 6, 5], 'Hide worked off: the flag hides nothing');
eq(idsW(view({ worked: true })), [1, 3, 6, 5], 'Hide worked drops worked senders, never the message to us');
eq(idsW(view({ worked: true }), involves), [1, 4, 3, 6, 5], 'nor a queued station\'s row');
eq(idsW(view({ worked: true, reply: true })), [1, 3, 6, 5], 'it combines with Replyable');
eq(idsW(view({ worked: true, sort: 'snr' })), [1, 3, 6, 5], 'and with a sort');
eq(sandbox.decodeViewOf(slot, view({ worked: true }), distKmOf).map((d) => d.id), [1, 2, 3, 4, 6, 5],
  'with no Worked lookup, nothing is worked');
// The closed menu names what is on.
assert.strictEqual(sandbox.decodeFilterLabel(view()), 'Filter');
assert.strictEqual(sandbox.decodeFilterLabel(view({ worked: true })), 'Hide worked');
assert.strictEqual(sandbox.decodeFilterLabel(view({ reply: true })), 'Replyable');
assert.strictEqual(sandbox.decodeFilterLabel(view({ park: true })), 'POTA/SOTA');
assert.strictEqual(sandbox.decodeFilterLabel(view({ reply: true, worked: true })), 'Filters · 2');
// Apply: the filters become exactly what is ticked, sort and mode kept; the view it started from
// is not changed, so Cancel (which never calls it) leaves the list as it was.
const before = view({ sort: 'snr', mode: 'FT8', reply: true });
const applied = sandbox.decodeViewWithFilters(before, { reply: false, park: true, worked: true });
eq(JSON.parse(JSON.stringify(applied)),
  { sort: 'snr', reply: false, park: true, worked: true, mode: 'FT8' }, 'Apply sets what is ticked, keeps sort and mode');
eq(before.reply, true, 'the view Apply started from is untouched');
eq(sandbox.decodeViewWithFilters(before, {}).reply, false, 'nothing ticked: every filter off');
assert.strictEqual(sandbox.decodeIsPark({ x: 'cq pota k1abc fn42' }), true);
assert.strictEqual(sandbox.decodeIsPark({ x: 'CQ POTAX K1ABC' }), false, 'a word boundary, not a prefix');
assert.strictEqual(sandbox.decodeIsPark({ x: 'CQ DX K1ABC' }), false);
console.log('PASS: decode sort and filter');
