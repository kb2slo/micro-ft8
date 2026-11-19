#!/usr/bin/env node
// The QRZ row on QSOs and the line under the key on Settings (docs/architecture/webapp.md, QRZ sync):
// runs app.html's own qrzRowModel() and qrzKeyLineModel(), which say what each reads, when it is amber,
// the one button the row shows and what a tap does. Also holds the markup to them: the row, the key and
// the sentence that says where the key goes.
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
const ctx = vm.createContext({});
vm.runInContext(extractFunction(src, 'qrzRowModel'), ctx);
vm.runInContext(extractFunction(src, 'qrzKeyLineModel'), ctx);
const row = (s) => JSON.parse(JSON.stringify(ctx.qrzRowModel(Object.assign({ key: true, phase: 'idle', sending: 0, result: null }, s))));
const looked = (r) => ({ result: Object.assign({ ok: true, logbook: 'K1ABC', held: 92, left: 0, otherCall: 0 }, r) });
const plain = { tone: '', glyph: '', act: '', chev: false };

// With no key the row leads to where one is typed, and is the only state with a chevron.
assert.deepStrictEqual(row({ key: false }), Object.assign({}, plain, { text: 'Not set up', tap: 'setup', chev: true }));
assert.deepStrictEqual(row({ key: false, phase: 'looking', result: looked({ left: 9 }).result }).text, 'Not set up', 'whatever else is held');

// A key and nothing known yet; a look and a send in hand offer no tap.
assert.deepStrictEqual(row({}), Object.assign({}, plain, { text: 'Not checked yet', tap: 'look' }));
assert.deepStrictEqual(row({ phase: 'looking' }), Object.assign({}, plain, { text: 'Checking…', tap: '' }));
assert.deepStrictEqual(row({ phase: 'sending', sending: 55 }), Object.assign({}, plain, { text: 'Sending 55…', tap: '' }));
assert.strictEqual(row(Object.assign({ phase: 'looking' }, looked({ left: 9 }))).act, '', 'no Send while a look runs');

// What a look found: nothing to send, or how many, with the page's one button.
assert.deepStrictEqual(row(looked({})), Object.assign({}, plain, { text: 'Up to date', tap: 'look', glyph: '✓' }));
assert.deepStrictEqual(row(looked({ left: 55 })), Object.assign({}, plain, { text: '55 to send', tap: 'send', act: 'Send' }));
assert.deepStrictEqual(row(looked({ left: 1 })).text, '1 to send');
assert.deepStrictEqual(row(looked({ held: 0, left: 0 })).text, 'Up to date', 'an empty log is up to date');

// A problem is amber, in words and a glyph as well as the tint, and says what a tap will do about it.
const amber = (text, tap) => Object.assign({}, plain, { text, tap, tone: 'amber', glyph: '!', chev: tap === 'setup' });
assert.deepStrictEqual(row({ result: { ok: false, error: 'auth' } }), amber('QRZ refused the key', 'setup'));
assert.deepStrictEqual(row({ result: { ok: false, error: 'unreachable' } }), amber('Cannot reach QRZ', 'look'));
assert.deepStrictEqual(row({ result: { ok: false, error: 'station' } }), amber('No reply from the station', 'look'));
assert.deepStrictEqual(row({ result: { ok: false, error: 'wait', waitMin: 4 } }), amber('Try again in 4 min', 'look'));
assert.deepStrictEqual(row({ result: { ok: false, error: 'too_many' } }), amber('Too many QSOs at QRZ to check', 'look'));
assert.deepStrictEqual(row({ result: { ok: false, error: 'qrz', reason: 'invalid option' } }), amber('QRZ: invalid option', 'look'));
assert.strictEqual(row({ result: { ok: false, error: 'qrz', reason: 'x'.repeat(200) } }).text.length, 'QRZ: '.length + 60, 'QRZ\'s words are kept to one line');
assert.deepStrictEqual(row({ result: { ok: false, error: 'something new' } }), amber('Could not check', 'look'));

// A key for another callsign's logbook: every QSO here is under a call that is not its own.
assert.deepStrictEqual(row(looked({ logbook: 'W9OTHER', held: 0, left: 0, otherCall: 147 })), amber('This key is W9OTHER\'s logbook', 'setup'));
assert.strictEqual(row(looked({ held: 80, left: 3, otherCall: 5 })).text, '3 to send', 'a few QSOs under another call do not make it the wrong logbook');
assert.strictEqual(row(looked({ held: 80, left: 0, otherCall: 5 })).text, 'Up to date');

// The line under the key.
const line = (s) => JSON.parse(JSON.stringify(ctx.qrzKeyLineModel(s)));
assert.deepStrictEqual(line({ state: 'none' }), { text: '', warn: false });
assert.deepStrictEqual(line({ state: 'checking' }), { text: 'Checking…', warn: false });
assert.deepStrictEqual(line({ state: 'ok', book: 'KB2SLO', count: 1310 }), { text: 'KB2SLO logbook · 1,310 QSOs', warn: false });
assert.deepStrictEqual(line({ state: 'ok', book: 'KB2SLO' }), { text: 'KB2SLO logbook', warn: false }, 'after a reload the count is not known');
assert.deepStrictEqual(line({ state: 'refused' }), { text: 'QRZ refused this key', warn: true });
assert.deepStrictEqual(line({ state: 'unchecked' }), { text: 'Saved. Cannot reach QRZ to check it', warn: true });

// The markup the models are drawn into, and the words that say where the key goes.
for (const piece of ['<button id=qrzRow type=button class=navrow>', '<span class=navmain>QRZ logbook</span>', 'id=qrzRowText', 'id=qrzRowAct',
  '<span>QRZ logbook key</span><input id=qrzKey', 'id=qrzKeyLine',
  'Kept in this browser only. It goes to QRZ through micro-ft8.kalikstein.com, which does not keep it.',
  '<script src=qrz_sync_core.js></script>']) {
  assert.ok(src.includes(piece), 'app.html holds: ' + piece);
}
// The row sits in the QSOs tab, above the list; the key in the Station page.
const log = src.slice(src.indexOf('<section id=view-log>'), src.indexOf('<section id=view-tools>'));
assert.ok(log.indexOf('id=qrzRow') > 0 && log.indexOf('id=qrzRow') < log.indexOf('id=logList'), 'the row is above the list');
const station = src.slice(src.indexOf('id=setp-station'), src.indexOf('id=setp-radio'));
assert.ok(station.includes('id=qrzKey'), 'the key is on the Station page');
console.log('ok');
