#!/usr/bin/env node
// What a band or frequency change, and the update, say they end (webapp.md, Band and frequency;
// Settings › Software update): runs app.html's own endsClauses() and endsSentence(). One sentence,
// shared by the retune and the update's review; only the update names a running CQ, since a retune
// leaves it calling on the new frequency.
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

const sb = {};
vm.createContext(sb);
vm.runInContext(['endsClauses', 'endsSentence', 'retuneEnds'].map((n) => extractFunction(src, n)).join('\n'), sb);
const say = (calls, tx, cq) => sb.endsSentence(sb.endsClauses(calls, tx, cq));

// Nothing to end: no sentence, and a retune goes ahead without asking.
assert.strictEqual(say([], false, false), '');
assert.deepStrictEqual(Array.from(sb.endsClauses([], false, false)), []);

// The retune's sentences, as they were under the native confirm.
assert.strictEqual(say(['K9ABC'], false, false), 'This ends your QSO with K9ABC.');
assert.strictEqual(say(['K9ABC'], true, false), 'This ends your QSO with K9ABC and stops the transmission on the air.');
assert.strictEqual(say(['K9ABC', 'W1AW'], false, false), 'This ends your 2 QSOs with K9ABC, W1AW.');
assert.strictEqual(say([], true, false), 'This stops the transmission on the air.');

// The update's, with a running CQ named first.
assert.strictEqual(say([], false, true), 'This ends your CQ.');
assert.strictEqual(say(['K9ABC'], true, true),
  'This ends your CQ, ends your QSO with K9ABC and stops the transmission on the air.');

// retuneEnds() reads the queue and the TX display: the CQ only when asked for, an aborted
// transmission not at all.
sb.queue = { 1: { dxcall: 'CQ' }, 2: { dxcall: 'K9ABC' } };
sb.txHud = { active: true, aborted: false };
assert.deepStrictEqual(Array.from(sb.retuneEnds()), ['ends your QSO with K9ABC', 'stops the transmission on the air']);
assert.deepStrictEqual(Array.from(sb.retuneEnds(true)),
  ['ends your CQ', 'ends your QSO with K9ABC', 'stops the transmission on the air']);
sb.queue = { 1: { dxcall: 'CQ' } };
sb.txHud = { active: true, aborted: true };
assert.deepStrictEqual(Array.from(sb.retuneEnds()), [], 'a retune leaves a lone CQ calling');
assert.deepStrictEqual(Array.from(sb.retuneEnds(true)), ['ends your CQ']);
sb.txHud = null;
sb.queue = {};
assert.deepStrictEqual(Array.from(sb.retuneEnds(true)), []);

// No native confirm() is left on the retune path: it often cancels silently on phones.
['changeBand', 'setOperatingBandFreq', 'switchToBandWithFreq', 'confirmRetune'].forEach(function (n) {
  assert(!/\bconfirm\(/.test(extractFunction(src, n)), n + ' must ask with askConfirm(), not confirm()');
});

console.log('ok');
