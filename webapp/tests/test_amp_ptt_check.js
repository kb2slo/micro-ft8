#!/usr/bin/env node
// Settings › Amplifier's PTT check (docs/architecture/webapp.md, Settings): runs app.html's own
// ampPttCheck() against the config lines the main board sends for the two radios read on the bench
// (2026-11-09): a mid-band QMX keyed for a PA100+, and a QMX+ with no PTT output on any band.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const app = path.join(__dirname, '..', 'app');
const src = fs.readFileSync(path.join(app, 'app.html'), 'utf8');
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

const ctx = {};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(app, 'bands.js'), 'utf8'), ctx);
vm.runInContext(extractFunction(src, 'ampPttCheck') + '\n' + extractFunction(src, 'radioTransmitBands'), ctx);

const midBand = {
  radio_model: 'QMX 60-15m',
  radio_bands: '60 40 30 20 17 15',
  radio_band_tx: '1 1 1 1 1 1',
  radio_ptt: '0100 0100 0100 0100 0100 0100',
  radio_ptt_serial: '0',
  active_bands: '40 20 15',
};
const qmxPlus = {
  radio_model: 'QMX+ 160-6m',
  radio_bands: '160 80 60 40 30 20 17 15 12 11 10 6',
  radio_ptt: '0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000',
  radio_ptt_serial: '0',
  active_bands: '40 20 15',
};
const check = (cfg) => ctx.ampPttCheck(cfg);

// Not set: nothing to check, whatever the radio says.
assert.strictEqual(check(Object.assign({}, qmxPlus, { amp_ptt: '0' })).state, 'off', 'amp_ptt 0 is off');
assert.strictEqual(check(Object.assign({}, qmxPlus)).state, 'off', 'no amp_ptt is off');

// Set, but the radio not read yet (just connected, or another radio).
assert.strictEqual(check({ amp_ptt: '1', active_bands: '40' }).state, 'unread', 'no radio lines: unread');

// The mid-band QMX keyed for a PA100+: right.
const ok = check(Object.assign({ amp_ptt: '1' }, midBand));
assert.strictEqual(ok.state, 'ok', 'mid-band QMX, grounded: ok');
assert.deepStrictEqual(Array.from(ok.lines), []);

// The same radio, but the amp said to key on +5V: each active band named, with what it has.
const five = check(Object.assign({ amp_ptt: '2' }, midBand));
assert.strictEqual(five.state, 'bad');
assert.deepStrictEqual(Array.from(five.lines),
  ['40 m: TX PTT grounded on', '20 m: TX PTT grounded on', '15 m: TX PTT grounded on']);

// The QMX+ as found: every active band has no PTT output; inactive bands are not named.
const plus = check(Object.assign({ amp_ptt: '1' }, qmxPlus));
assert.strictEqual(plus.state, 'bad', 'QMX+, grounded: bad');
assert.deepStrictEqual(Array.from(plus.lines),
  ['40 m: no PTT output on', '20 m: no PTT output on', '15 m: no PTT output on']);

// No active_bands means every band: all twelve named.
assert.strictEqual(check(Object.assign({ amp_ptt: '1' }, qmxPlus, { active_bands: '' })).lines.length, 12);

// An RX output on too keys the amp on receive: named, though TX PTT grounded is right.
const rx = check(Object.assign({ amp_ptt: '1' }, midBand, { radio_ptt: '0100 0101 0100 0100 0100 0100' }));
assert.deepStrictEqual(Array.from(rx.lines), ['40 m: TX PTT grounded, RX PTT grounded on']);

// An unread flag is said as such.
const unread = check(Object.assign({ amp_ptt: '1' }, midBand, { radio_ptt: '0100 01?0 0100 0100 0100 0100' }));
assert.deepStrictEqual(Array.from(unread.lines), ['40 m: not read']);

// The PTT jack as a serial port: said first, whatever the bands.
const serial = check(Object.assign({ amp_ptt: '1' }, midBand, { radio_ptt_serial: '1' }));
assert.strictEqual(serial.state, 'bad');
assert.ok(/Serial 6 on PTT/.test(serial.lines[0]), 'serial named');
assert.strictEqual(serial.lines.length, 1);

// An active band the radio does not have is not this check's (row vcng's band ceiling).
assert.strictEqual(check(Object.assign({ amp_ptt: '1' }, midBand, { active_bands: '80 40' })).state, 'ok');

// Set it writes the bands the radio transmits on, as its Transmit flags say: the operator's choice.
assert.deepStrictEqual(Array.from(ctx.radioTransmitBands({ radio_bands: '60 40 30', radio_band_tx: '0 1 1' })), ['40', '30']);
assert.deepStrictEqual(Array.from(ctx.radioTransmitBands({})), []);

console.log('amp PTT check: ok');
