#!/usr/bin/env node
// Decode sort and filter (docs/architecture/webapp.md, Decode sort and filter): runs the real functions from app.html, as
// test_decode_batching.js does. Within a slot; filters combine; a message to us stays on top and
// is never hidden; a decode with no grid sorts last under Nearest and Farthest; Hide worked drops
// what the Worked flag marks; the Filter pill's label names what is on, and the sheet's Apply sets
// exactly what is ticked; the bar shows once a decode is listed, even if every row is filtered out.
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
vm.runInContext("var DV_FILTERS = ['reply', 'park'];\nvar DV_MODES = ['all', 'FT8', 'FT4'];\n" +
  ['DV_NEEDS', 'DV_NEED_HIDES', 'DV_NEED_LABELS'].map((n) => src.match(new RegExp('var ' + n + ' = [^;]*;'))[0]).join('\n') + '\n' +
  // listenedChannels() stands in for the drawer's channels: the test sets `listening` to what it needs.
  "var listening = [];\nfunction listenedChannels(cfg) { return listening; }\n" +
  ['workedMark', 'decodeIsPark', 'isReplyableDecode', 'decodePassesView', 'decodeViewOf', 'decodeFiltersOn', 'decodeFilterLabel',
   'decodeViewWithFilters', 'decodeBarShown', 'decodeModeFilterOn', 'decodeViewInForce']
  .map((n) => extractFunction(src, n)).join('\n'), sandbox);
assert(src.includes("var DV_FILTERS = ['reply', 'park'];"), 'DV_FILTERS changed: update this test');

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
const view = (o) => Object.assign({ sort: 'default', reply: false, park: false, need: 'all' }, o);
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
// The marks (workedMark()), from the main board's worked bits: 1 dupe, 2 this band, 4 any band; the first that holds.
eq([undefined, null, 0, 4, 6, 7, 3].map((wk) => sandbox.workedMark(wk)), ['', '', 'new', 'newband', 'worked', 'dupe', 'dupe'],
  'none known; New; New on band; Worked; Dupe');
// The filter on them: each choice hides what the one before hid, never the message to us or a queued station's
// row, nor a decode whose mark is not known.
const marks = { 1: 'dupe', 2: 'dupe', 3: 'new', 4: 'worked', 6: 'newband', 5: '' };
const idsW = (v, inv) => sandbox.decodeViewOf(slot, v, distKmOf, inv, (d) => marks[d.id]).map((d) => d.id);
eq(idsW(view()), [1, 2, 3, 4, 6, 5], 'All: the marks hide nothing');
eq(idsW(view({ need: 'nodupes' })), [1, 3, 4, 6, 5], 'No dupes drops the dupe, never the message to us');
eq(idsW(view({ need: 'needed' })), [1, 3, 6, 5], 'Needed drops the worked too, keeps New and New on band');
eq(idsW(view({ need: 'new' })), [1, 3, 5], 'New only keeps the never worked, and the unknown');
eq(idsW(view({ need: 'new' }), involves), [1, 4, 3, 5], 'a queued station\'s row stays whatever its mark');
eq(idsW(view({ need: 'needed', reply: true })), [1, 3, 6, 5], 'it combines with Replyable');
eq(idsW(view({ need: 'needed', sort: 'snr' })), [1, 3, 6, 5], 'and with a sort');
eq(sandbox.decodeViewOf(slot, view({ need: 'new' }), distKmOf).map((d) => d.id), [1, 2, 3, 4, 6, 5],
  'with no marks known, nothing is hidden');
// The closed menu names what is on.
assert.strictEqual(sandbox.decodeFilterLabel(view()), 'Filter');
assert.strictEqual(sandbox.decodeFilterLabel(view({ need: 'nodupes' })), 'No dupes');
assert.strictEqual(sandbox.decodeFilterLabel(view({ need: 'needed' })), 'Needed');
assert.strictEqual(sandbox.decodeFilterLabel(view({ need: 'new' })), 'New only');
assert.strictEqual(sandbox.decodeFilterLabel(view({ reply: true })), 'Replyable');
assert.strictEqual(sandbox.decodeFilterLabel(view({ park: true })), 'POTA/SOTA');
assert.strictEqual(sandbox.decodeFilterLabel(view({ reply: true, need: 'needed' })), 'Filters · 2');
// Apply: the filters become exactly what is ticked, sort and mode kept; the view it started from
// is not changed, so Cancel (which never calls it) leaves the list as it was.
const before = view({ sort: 'snr', mode: 'FT8', reply: true });
const applied = sandbox.decodeViewWithFilters(before, { reply: false, park: true, need: 'needed' });
eq(JSON.parse(JSON.stringify(applied)),
  { sort: 'snr', reply: false, park: true, need: 'needed', mode: 'FT8' }, 'Apply sets what is picked, keeps sort and mode');
eq(before.reply, true, 'the view Apply started from is untouched');
eq(sandbox.decodeViewWithFilters(before, {}).reply, false, 'nothing ticked: every filter off');
// The bar shows once a decode is listed, whatever the filters hide, so a filter that hides every row
// cannot take away the control that turns it off.
assert.strictEqual(sandbox.decodeBarShown([]), false, 'no slots yet: no bar');
assert.strictEqual(sandbox.decodeBarShown([[]]), false, 'an empty batch: no bar');
assert.strictEqual(sandbox.decodeBarShown([[], slot]), true, 'a decode listed: the bar');
const allHidden = sandbox.decodeViewOf([slot[3]], view({ reply: true, park: true }), distKmOf);
eq(allHidden.length, 0, 'these filters hide every row of this slot');
assert.strictEqual(sandbox.decodeBarShown([[slot[3]]]), true, 'and the bar still shows');
assert.strictEqual(sandbox.decodeIsPark({ x: 'cq pota k1abc fn42' }), true);
assert.strictEqual(sandbox.decodeIsPark({ x: 'CQ POTAX K1ABC' }), false, 'a word boundary, not a prefix');
assert.strictEqual(sandbox.decodeIsPark({ x: 'CQ DX K1ABC' }), false);
// The mode filter: FT8 only or FT4 only hides the other mode's batches, but never a message to us or about our queue.
const both = [{ mode: 'FT8' }, { mode: 'FT4' }];
const idsM = (v, mode, inv) => sandbox.decodeViewOf(slot, v, distKmOf, inv, null, mode).map((d) => d.id);
eq(idsM(view({ mode: 'FT4' }), 'FT4'), [1, 2, 3, 4, 6, 5], 'FT4 only: an FT4 batch is all shown');
eq(idsM(view({ mode: 'FT4' }), 'FT8'), [1], 'FT4 only: an FT8 batch keeps only the message to us');
eq(idsM(view({ mode: 'FT4' }), 'FT8', (d) => d.id === 4), [1, 4], '... and a row about our queue');
eq(idsM(view({ mode: 'all' }), 'FT8'), [1, 2, 3, 4, 6, 5], 'Both: nothing hidden');
eq(idsM(view({ mode: 'FT8', reply: true }), 'FT8'), [1, 2, 3, 6, 5], 'with another filter, both apply');
// In force only while both channels are listened to.
sandbox.listening = both;
assert.strictEqual(sandbox.decodeViewInForce(view({ mode: 'FT4' }), {}).mode, 'FT4', 'both listened to: in force');
sandbox.listening = [{ mode: 'FT8' }];
assert.strictEqual(sandbox.decodeViewInForce(view({ mode: 'FT4' }), {}).mode, 'all', 'one channel: set aside');
assert.strictEqual(sandbox.decodeModeFilterOn({}), false, 'and not offered');
// The pill's label, and the sheet's pick.
assert.strictEqual(sandbox.decodeFilterLabel(view({ mode: 'FT4' })), 'FT4 only');
assert.strictEqual(sandbox.decodeFilterLabel(view({ mode: 'FT8', need: 'nodupes' })), 'Filters · 2');
assert.strictEqual(sandbox.decodeFilterLabel(view({ mode: 'all' })), 'Filter');
eq(sandbox.decodeViewWithFilters(view({ mode: 'FT4' }), { reply: true }).mode, 'FT4', 'a sheet without the mode rows keeps it');
eq(sandbox.decodeViewWithFilters(view({ mode: 'FT4' }), { mode: 'all' }).mode, 'all', 'a pick of Both clears it');
// The mode rows sit in the Filter sheet, after the filters, and are hidden until both channels are listened to.
const fs4 = src.indexOf('<div id=dvFilterMode role=radiogroup aria-label=Mode hidden>');
assert(fs4 > src.indexOf('<div id=dvFilterNeed') && fs4 < src.indexOf('<div class=dvfbtns>', fs4), 'the mode row, in the Filter sheet, after the marks');
const fn = src.indexOf('<div id=dvFilterNeed');
assert.strictEqual((src.slice(fn, src.indexOf('</div>', fn)).match(/class="dvfopt dvfseg" data-need=/g) || []).length, 4, 'the marks: one row of four');
assert.strictEqual((src.slice(fs4, src.indexOf('</div>', fs4)).match(/class="dvfopt dvfseg"/g) || []).length, 3, 'one row of three segments');
console.log('PASS: decode sort and filter');
