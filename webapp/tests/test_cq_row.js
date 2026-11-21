#!/usr/bin/env node
// The CQ row's channel (docs/architecture/webapp.md, The CQ row): app.html's own cqChannels(), cqChannelFor(),
// cqCallBody(), cqStatusText() and slotBarPeriodMs(). Call CQ names a channel, its mode and dial in Hz, as the
// main board does (porta_proto_parse_cq_body()); it calls on the channels listened to (listenedChannels()); the
// status line names the mode while there are two, and an FT4 CQ's periods; the slot bar counts the armed TX's
// mode's slots.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');
function extract(name) {
  const start = src.indexOf('function ' + name + '(');
  assert(start !== -1, name + ' not found in app.html');
  return src.slice(start, src.indexOf('\n}\n', start) + 3);
}
const ctx = vm.createContext({});
// The real band table, and bandFreq(), which reads a bandN override from CONFIG.
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'bands.js'), 'utf8'), ctx);
vm.runInContext('var SLOT_MS = 15000; var QMX_FT4_BESIDE_MIN_HZ = -33000, QMX_FT4_BESIDE_MAX_HZ = 6000;' +
  ' var LISTEN_FT8 = 1, LISTEN_FT4 = 2, LISTEN_BOTH = 3;', ctx);
['bandFreq', 'currentFt4DialKhz', 'currentDockFreqKhz', 'listenMask', 'bandFt4Khz', 'bandBothFit', 'bandChannels',
 'operatingBandIdx', 'listenedChannels', 'cqChannels', 'cqChannelFor', 'cqCallBody', 'cqStatusText',
 'slotBarPeriodMs'].forEach(function (n) { vm.runInContext(extract(n), ctx); });
const B20 = '5', B40 = '3';

const plain = (v) => JSON.parse(JSON.stringify(v));

// Channels.
assert.deepStrictEqual(plain(ctx.cqChannels({})), [], 'no config yet: nothing to call on');
assert.deepStrictEqual(plain(ctx.cqChannels({ band_sel: B20, listen5: '1' })), [{ mode: 'FT8', dial_hz: 14074000 }],
                       'FT8 alone while FT4 is not decoded');
assert.deepStrictEqual(plain(ctx.cqChannels({ band_sel: B20, listen5: '3' })),
                       [{ mode: 'FT8', dial_hz: 14074000 }, { mode: 'FT4', dial_hz: 14080000 }],
                       'FT8 and FT4 at their dials while FT4 is decoded');
assert.deepStrictEqual(plain(ctx.cqChannels({ band_sel: B40, listen3: '3' })),
                       [{ mode: 'FT8', dial_hz: 7074000 }, { mode: 'FT4', dial_hz: 7047500 }],
                       "40 m's FT4 dial, below FT8's, in whole Hz");
assert.deepStrictEqual(plain(ctx.cqChannels({ band_sel: B20, band5: '14075.5', listen5: '1' })),
                       [{ mode: 'FT8', dial_hz: 14075500 }], "a band's own dial, as the main board sets it");

// The chosen mode falls back to FT8 when FT4 is not listened to.
const both = { band_sel: B20, listen5: '3' };
assert.strictEqual(ctx.cqChannelFor(both, 'FT4').mode, 'FT4');
assert.strictEqual(ctx.cqChannelFor({ band_sel: B20, listen5: '1' }, 'FT4').mode, 'FT8',
                   'a remembered FT4 calls in FT8 once FT4 is not listened to');
assert.deepStrictEqual(plain(ctx.cqChannelFor({ band_sel: '7', listen7: '2' }, 'FT8')), { mode: 'FT4', dial_hz: 21140000 },
                       'FT4 alone on 15 m: the CQ calls in FT4 at 21.140 MHz');
assert.strictEqual(ctx.cqChannelFor({}, 'FT8'), null, 'no channel before the first config');

// The body the sidekick takes.
assert.strictEqual(ctx.cqCallBody({ mode: 'FT4', dial_hz: 14080000 }), 'FT4 14080000');
assert.strictEqual(ctx.cqCallBody({ mode: 'FT8', dial_hz: 7074000 }), 'FT8 7074000');

// The status line.
const cq8 = { dxcall: 'CQ', parity: 0, mode: 0 };
const cq4 = { dxcall: 'CQ', parity: 1, mode: 1 };
assert.strictEqual(ctx.cqStatusText(null, {}, true), 'Not calling');
assert.strictEqual(ctx.cqStatusText(cq8, { 1: cq8 }, false), 'Calling · even slots', 'FT8 alone: as before');
assert.strictEqual(ctx.cqStatusText(cq8, { 1: cq8 }, true), 'Calling · FT8 · even slots', 'beside FT4: names FT8');
assert.strictEqual(ctx.cqStatusText(cq4, { 1: cq4 }, true), 'Calling · FT4 · odd periods', "FT4's periods");
assert.strictEqual(ctx.cqStatusText(cq4, { 1: cq4, 2: { dxcall: 'K1ABC' } }, true),
                   'Waits for the queue · FT4 · odd periods', 'a QSO goes first');

// The slot bar's period.
assert.strictEqual(ctx.slotBarPeriodMs(null), 15000, 'nothing heard: FT8 slots');
assert.strictEqual(ctx.slotBarPeriodMs({ sp: 0, hz: 0, md: 0 }), 15000);
assert.strictEqual(ctx.slotBarPeriodMs({ sp: 1, hz: 1500, md: 1 }), 7500, "an armed FT4 TX: FT4's periods");

// The mode button sits before Call CQ in the row.
const row = src.indexOf('<div id=opCqRow>');
assert(row > 0 && src.indexOf('id=cqModeBtn', row) < src.indexOf('id=cqCallBtn', row), 'the mode button, then Call CQ');
console.log('PASS: CQ row');
