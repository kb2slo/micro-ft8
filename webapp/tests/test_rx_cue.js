// The slot bar's receive cues and the HL2's radio chip: app.html's own rxCue() and radioChipState(), so the bar
// says why a slot is not decoding for either radio, in the order the cues are checked.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');
function extractFunction(text, name) {
  const start = text.indexOf('function ' + name + '(');
  if (start < 0) throw new Error('no function ' + name);
  let depth = 0, i = text.indexOf('{', start);
  for (; i < text.length; i++) {
    if (text[i] === '{') depth++;
    else if (text[i] === '}' && --depth === 0) { i++; break; }
  }
  return text.slice(start, i);
}
const ctx = { usbState: null, everCatUp: false };
vm.createContext(ctx);
vm.runInContext(['yn', 'radioReadBack', 'radioChipState', 'rxCue'].map((n) => extractFunction(src, n)).join('\n'), ctx);

const hl2 = (state, extra) => Object.assign({ radio: '6', cat_ready: '0', audio_rx: '0', decode_failed: '0',
  hl2: state, hl2_overload: '0' }, extra || {});

// The HL2: each transport state is its own reason, on the bar and the chip.
assert.strictEqual(ctx.rxCue(hl2('nolink')), 'No audio · no Ethernet link');
assert.strictEqual(ctx.rxCue(hl2('silent')), 'No audio · HL2 not answering');
assert.strictEqual(ctx.rxCue(hl2('busy')), 'No audio · HL2 busy, another program has it');
assert.strictEqual(ctx.rxCue(hl2('lost')), 'No audio · stream lost');
assert.strictEqual(ctx.rxCue(hl2('found')), 'No audio · HL2 found, starting');
assert.strictEqual(ctx.rxCue(hl2('streaming', { audio_rx: '1' })), '', 'streaming and decoding: nothing to say');
assert.strictEqual(ctx.rxCue(hl2('streaming', { audio_rx: '1', hl2_overload: '1' })), 'Overload · ADC clipping');
assert.strictEqual(ctx.rxCue(hl2('streaming', { audio_rx: '1', decode_failed: '1', hl2_overload: '1' })),
  'Not decoding · decoder could not start', 'the decoder outranks overload');
assert.strictEqual(ctx.radioChipState(hl2('nolink')), 'none');
assert.strictEqual(ctx.radioChipState(hl2('silent')), 'none');
assert.strictEqual(ctx.radioChipState(hl2('busy')), 'busy');
assert.strictEqual(ctx.radioChipState(hl2('lost')), 'streamlost', 'lost is red, like CAT lost');
assert.strictEqual(ctx.radioChipState(hl2('found')), 'found');
assert.strictEqual(ctx.radioChipState(hl2('streaming', { audio_rx: '1' })), 'rxonly');

// The QMX: no audio, and why, from its USB presence and CAT.
const qmx = (extra) => Object.assign({ radio: '2', cat_ready: '1', audio_rx: '1', decode_failed: '0' }, extra || {});
assert.strictEqual(ctx.rxCue(qmx()), '', 'the QMX decoding: nothing to say');
assert.strictEqual(ctx.rxCue(qmx({ cat_ready: '0', audio_rx: '0' }), { present: false }), 'No audio · QMX not connected');
assert.strictEqual(ctx.rxCue(qmx({ audio_rx: '0' }), { present: true }), 'No audio · QMX audio down');
assert.strictEqual(ctx.rxCue(qmx({ cat_ready: '0', audio_rx: '0' }), { present: true }), 'No audio · QMX connecting');
assert.strictEqual(ctx.rxCue(qmx({ decode_failed: '1' })), 'Not decoding · decoder could not start');
console.log('rx cue: all checks passed');
