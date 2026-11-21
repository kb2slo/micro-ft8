#!/usr/bin/env node
// The channels listened to (docs/architecture/webapp.md, Band and frequency): app.html's own listenMask(),
// bandBothFit(), bandChannels(), listenedChannels(), listenChipTap() and listenedLabel(), against the real
// bands.js. The same cases as station_plan() in host_mock/test_channel_entries.cpp: FT8 alone by default, FT4
// beside FT8 where a QMX's window holds both, FT4 alone on any band, 15 m and 10 m included, and both where they
// do not fit read as FT8 alone. The app's window bounds are held to qmx_iq_channel.h's numbers.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const root = path.join(__dirname, '..', '..');
const src = fs.readFileSync(path.join(root, 'webapp', 'app', 'app.html'), 'utf8');
function extract(name) {
  const start = src.indexOf('function ' + name + '(');
  assert(start !== -1, name + ' not found in app.html');
  return src.slice(start, src.indexOf('\n}\n', start) + 3);
}
function constLine(name) {
  const m = src.match(new RegExp('var ' + name + ' = [^;]+;'));
  assert(m, name + ' not found in app.html');
  return m[0];
}
const ctx = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(root, 'webapp', 'app', 'bands.js'), 'utf8'), ctx);
vm.runInContext([constLine('QMX_FT4_BESIDE_MIN_HZ'), constLine('QMX_FT4_BESIDE_MAX_HZ'),
                 'var LISTEN_FT8 = 1, LISTEN_FT4 = 2, LISTEN_BOTH = 3;'].join('\n'), ctx);
['bandFreq', 'khzToMhzLabel', 'listenMask', 'bandFt4Khz', 'bandBothFit', 'bandChannels', 'operatingBandIdx', 'listenedChannels',
 'listenChipTap', 'listenedLabel'].forEach(function (n) { vm.runInContext(extract(n), ctx); });
const plain = (v) => JSON.parse(JSON.stringify(v));
const B = { m40: 3, m20: 5, m15: 7, m10: 9 };

// The window: qmx_iq_channel.h puts FT8's channel 13.5 kHz above the LO, a channel 1.5 kHz either side, and the
// window's edge 21 kHz from the LO. FT4's dial may then sit from 33 kHz below FT8's to 6 kHz above it.
const hdr = fs.readFileSync(path.join(root, 'components', 'radio_control', 'include', 'qmx_iq_channel.h'), 'utf8');
const num = (n) => parseFloat(hdr.match(new RegExp(n + ' = ([0-9.]+)'))[1]);
const centre = num('kQmxIqFt8CenterHz'), edge = num('kQmxIqWindowEdgeHz'), half = num('kQmxIqChannelHalfHz');
assert.strictEqual(ctx.QMX_FT4_BESIDE_MAX_HZ, edge - half - centre, "the app's upper bound is the header's");
assert.strictEqual(ctx.QMX_FT4_BESIDE_MIN_HZ, -(edge - half) - centre, "the app's lower bound is the header's");

// Which bands hold both on a QMX: every band but 15 m and 10 m.
const fits = ctx.BANDS.filter((b) => ctx.bandBothFit({ radio: '2' }, b.i)).map((b) => b.name);
assert.deepStrictEqual(plain(fits), ['160m', '80m', '60m', '40m', '30m', '20m', '17m', '12m', '6m', '2m'],
                       'a QMX hears FT8 and FT4 together on every band but 15 m and 10 m');
assert.strictEqual(ctx.bandBothFit({ radio: '6' }, B.m20), false, 'an HL2 hears one channel at a time');

// The channels.
assert.deepStrictEqual(plain(ctx.bandChannels({}, B.m20)),
  [{ mode: 'FT8', khz: 14074, dial_hz: 14074000, on: true }, { mode: 'FT4', khz: 14080, dial_hz: 14080000, on: false }],
  'FT8 alone by default');
assert.deepStrictEqual(plain(ctx.bandChannels({ listen5: '3' }, B.m20)).map((c) => c.on), [true, true], 'both on 20 m');
assert.deepStrictEqual(plain(ctx.bandChannels({ listen7: '2' }, B.m15)).map((c) => c.on), [false, true],
                       'FT4 alone on 15 m');
assert.deepStrictEqual(plain(ctx.bandChannels({ listen7: '3' }, B.m15)).map((c) => c.on), [true, false],
                       'both where they do not fit read as FT8 alone, as the main board plans it');
assert.deepStrictEqual(plain(ctx.bandChannels({ listen5: '3', radio: '6' }, B.m20)).map((c) => c.on), [true, false],
                       'and on an HL2');
assert.deepStrictEqual(plain(ctx.bandChannels({ listen5: '9' }, B.m20)).map((c) => c.on), [true, false],
                       'anything but 1, 2 or 3 is FT8 alone');
assert.deepStrictEqual(plain(ctx.bandChannels({}, 99)), [], 'no band, no channels');
assert.deepStrictEqual(plain(ctx.listenedChannels({ band_sel: '9', listen9: '2' })),
                       [{ mode: 'FT4', khz: 28180, dial_hz: 28180000, on: true }], 'FT4 alone on 10 m, at 28.180');

// A tap on a chip.
const tap = (cfg, idx, mode) => plain(ctx.listenChipTap(cfg, idx, mode));
assert.deepStrictEqual(tap({}, B.m20, 'FT4'), { mask: 3, note: '' }, 'adding FT4 where it fits listens to both');
assert.deepStrictEqual(tap({ listen5: '3' }, B.m20, 'FT8'), { mask: 2, note: '' }, 'dropping FT8 leaves FT4 alone');
assert.strictEqual(tap({}, B.m20, 'FT8').mask, 1, 'the last channel stays on');
assert.strictEqual(tap({}, B.m20, 'FT8').note, 'The station listens to at least one channel.');
const sw = tap({}, B.m15, 'FT4');
assert.strictEqual(sw.mask, 2, 'on 15 m, FT4 replaces FT8');
assert.strictEqual(sw.note, 'On 15m FT8 and FT4 are 66 kHz apart, and the radio hears one at a time: FT4 replaces FT8, ' +
                   'and the dial moves to 21.140 MHz.');
assert.strictEqual(tap({ listen9: '2' }, B.m10, 'FT8').mask, 1, 'on 10 m, FT8 replaces FT4');

// FT4's own dial (ft4band<N>): it moves FT4's channel and decides whether both fit.
assert.strictEqual(ctx.bandFt4Khz({}, B.m20), 14080, 'unset: the band\'s standard FT4 dial');
assert.strictEqual(ctx.bandFt4Khz({ ft4band5: '14079.5' }, B.m20), 14079.5, 'set: its own');
assert.deepStrictEqual(plain(ctx.bandChannels({ listen5: '3', ft4band5: '14079.5' }, B.m20))[1],
                       { mode: 'FT4', khz: 14079.5, dial_hz: 14079500, on: true }, "FT4's channel at its own dial");
assert.strictEqual(ctx.bandBothFit({ ft4band5: '14080' }, B.m20), true, 'FT4 6 kHz above FT8: the edge, inside');
assert.strictEqual(ctx.bandBothFit({ ft4band5: '14081' }, B.m20), false, 'FT4 7 kHz above FT8: outside');
assert.deepStrictEqual(plain(ctx.bandChannels({ listen5: '3', ft4band5: '14090' }, B.m20)).map((c) => c.on), [true, false],
                       'both with FT4 moved out of the window read as FT8 alone');
assert.deepStrictEqual(plain(ctx.listenedChannels({ band_sel: '7', listen7: '2', ft4band7: '21142' })),
                       [{ mode: 'FT4', khz: 21142, dial_hz: 21142000, on: true }], 'FT4 alone on 15 m at a dial of its own');

// The radio bar's channels.
assert.strictEqual(ctx.listenedLabel({ band_sel: '5' }), 'FT8 14.074 MHz');
assert.strictEqual(ctx.listenedLabel({ band_sel: '5', listen5: '3' }), 'FT8 14.074 · FT4 14.080');
assert.strictEqual(ctx.listenedLabel({ band_sel: '3', listen3: '3' }), 'FT8 7.074 · FT4 7.0475');
assert.strictEqual(ctx.listenedLabel({}), '', 'no config yet');

// The chips sit under the band grid, above the frequency row.
const grid = src.indexOf('<div id=opBandGrid');
assert(grid > 0 && src.indexOf('id=opListenChips', grid) < src.indexOf('<div id=opFreqRow class=frqrow>', grid),
       'the chips, between the band grid and the frequency row');
// A frequency row per channel: FT8's, then FT4's, each labelled with its mode.
const r8 = src.indexOf('<div id=opFreqRow class=frqrow>'), r4 = src.indexOf('<div id=opFt4FreqRow class=frqrow hidden>');
assert(r8 > 0 && r4 > r8, "FT8's row, then FT4's, hidden until FT4 is listened to");
assert(src.indexOf('<span class=frqmode>FT4</span>', r4) - r4 < 80, "FT4's row names its mode");
assert(/FT4: \{ row: 'opFt4FreqRow'[^}]*key: 'ft4band'/.test(src), "FT4's row writes ft4band<N>");
assert(/FT8: \{ row: 'opFreqRow'[^}]*key: 'band'/.test(src), "FT8's row writes band<N>");
console.log('PASS: channels');
