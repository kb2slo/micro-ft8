#!/usr/bin/env node
// The radio bar's state priority (webapp.md, The radio bar): runs app.html's own
// radioBarState() and noteTxHud(). On the air outranks everything, a radio problem included (CAT
// lost mid-transmission keeps Transmitting and its Stop); a transmission that did not go outranks
// no contact with the station, which outranks a radio problem; a problem outranks Ready. Stop is offered only while something is on the air.
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
vm.runInContext([/var TX_NO_RF_AFTER_MS = \d+;/.exec(src)[0], /var NO_MAIN_TITLE = .*;/.exec(src)[0], /var NO_MAIN_HINT = .*;/.exec(src)[0]].concat(['radioBarState', 'noteTxHud', 'utcDayLeft', 'txReadout']
  .map((n) => extractFunction(src, n))).join('\n'), sb);

const ctx = (o) => Object.assign({ tune: false, ourCq: false, band: '20m', freq: '14.074', radio: 'QMX',
                                   swrTrips: 0, act: '', wantMHz: '', linkLost: false, unpaired: false }, o || {});
const tx = (o) => Object.assign({ active: true, aborted: false, nk: false, wrblk: false, pw: 49, swr: 130,
                                  batt: -1, hz: 1500, reason: '', x: 'K9ABC KB2SLO R-09' }, o || {});
const bar = (chip, hud, o) => sb.radioBarState(chip, hud, ctx(o));

// 4. Ready is quiet, with the activation's refs and countdown when there is one.
let s = bar('ready', null);
assert.strictEqual(s.state, 'ready');
assert.strictEqual(s.tone, 'quiet');
assert.strictEqual(s.main, '20m · 14.074 MHz');
assert.strictEqual(s.sub, 'QMX ready');
assert.strictEqual(s.label, 'Band & radio');
assert.strictEqual(s.stop, '', 'nothing on the air, no Stop');
assert.strictEqual(bar('ready', null, { act: ' · 3 refs · 0000Z 3h12m' }).sub, 'QMX ready · 3 refs · 0000Z 3h12m');

// 3. A radio problem replaces Ready; its label is the way to its actions.
assert.deepStrictEqual(['found', 'drift', 'lost', 'none', 'unknown', 'nodecode'].map((c) => bar(c, null).tone),
  ['amber', 'amber', 'red', 'grey', 'grey', 'red']);
assert.strictEqual(bar('drift', null, { wantMHz: '14.08' }).sub, 'Radio is on 14.08 · tap to fix');
assert.strictEqual(bar('drift', null).label, 'Review ›');
assert.strictEqual(bar('lost', null).label, 'Radio ›');
assert.strictEqual(bar('lost', null).sub, 'CAT lost · cannot retune');
assert.strictEqual(bar('found', null).sub, 'QMX found · syncing');
assert.strictEqual(bar('none', null).main, '20m · —', 'no radio: no frequency to claim');

// 1. On the air outranks everything, with power and SWR large and Stop its own target.
s = bar('ready', tx());
assert.strictEqual(s.state, 'tx');
assert.strictEqual(s.tone, 'red');
assert.strictEqual(s.main, '20m · 14.074 MHz', 'the band stays; power and SWR are in the slot bar');
assert.strictEqual(s.sub, 'Sending K9ABC KB2SLO R-09 · 1500 Hz');
assert.strictEqual(s.stop, 'Stop TX');
assert.strictEqual(s.label, '', 'Stop takes the label\'s place');
assert.strictEqual(bar('ready', tx({ x: 'CQ KB2SLO FN30' }), { ourCq: true }).stop, 'Stop CQ');
// The readout inside the slot bar (txReadout()).
const ro = (hud, tuning, ms) => sb.txReadout(tx(hud), !!tuning, ms === undefined ? 5000 : ms);
assert.strictEqual(ro({}).text, '4.9 W · SWR 1.30');
assert.strictEqual(ro({}).dead, false);
assert.strictEqual(ro({ pw: -1, swr: -1 }).text, '— W · SWR —', 'no reading yet is not zero');
assert.strictEqual(ro({ wrblk: true }).text, 'Write blocked');
assert.strictEqual(ro({ x: 'TUNE' }, true).text, 'Tune · 4.9 W · SWR 1.30');
assert.strictEqual(ro({ pw: 0, swr: 445 }, false, sb.TX_NO_RF_AFTER_MS + 1).text, '0.0 W · no RF',
  'keyed past the first tone and meter reply at 0 W: no RF, and its SWR means nothing');
assert.strictEqual(ro({ pw: 0, swr: 445 }, false, sb.TX_NO_RF_AFTER_MS + 1).dead, true);
assert.strictEqual(ro({ pw: 0, swr: 163 }, false, 600).dead, false, 'the first poll comes before the first tone');
s = bar('ready', tx({ x: 'TUNE' }), { tune: true });
assert.strictEqual(s.state, 'tune');
assert.strictEqual(s.stop, 'Stop Tune');
assert(s.sub.indexOf('Tuning') === 0);

// The open question, decided 2026-10-25: CAT lost mid-transmission keeps Transmitting and Stop.
for (const chip of ['lost', 'drift', 'found', 'nodecode', 'none']) {
  s = bar(chip, tx());
  assert.strictEqual(s.state, 'tx', chip + ' mid-transmission keeps Transmitting');
  assert.strictEqual(s.stop, 'Stop TX', chip + ' mid-transmission keeps Stop');
}
assert(/· CAT lost$/.test(bar('lost', tx()).sub), 'and the sub-line says CAT is lost');
assert(!/CAT lost/.test(bar('ready', tx()).sub));

// 3, first: no contact with the station outranks a radio problem and Ready, but not the air.
s = bar('lost', null, { linkLost: true });
assert.strictEqual(s.state, 'nolink');
assert.strictEqual(s.tone, 'red');
assert.strictEqual(s.main, 'No contact with the station');
assert.strictEqual(bar('ready', null, { linkLost: true }).state, 'nolink');
assert.strictEqual(bar('ready', tx(), { linkLost: true }).state, 'tx', 'on the air keeps its Stop');
assert.strictEqual(bar('ready', null, { linkLost: false }).state, 'ready');

// Not paired comes after no contact and before a radio problem; on the air still wins.
s = bar('lost', null, { unpaired: true });
assert.strictEqual(s.state, 'unpaired');
assert.strictEqual(s.tone, 'amber');
assert.strictEqual(s.sub, 'Not paired · press the sidekick’s button');
assert.strictEqual(bar('ready', null, { unpaired: true, linkLost: true }).state, 'nolink');

assert.strictEqual(bar('ready', tx(), { unpaired: true }).state, 'tx');

// A sidekick with no main board (webapp.md, The radio bar): its own state, saying what to check. It outranks No
// contact, which cannot say which end is missing, and Not paired, since pairing would change nothing yet.
s = bar('none', null, { noMain: true });
assert.deepStrictEqual([s.state, s.tone, s.main, s.sub, s.stop],
  ['nomain', 'red', 'Main board not connected', 'Check the cable between the boards and the main board\u2019s power', '']);
assert.strictEqual(bar('ready', null, { noMain: true, linkLost: true }).state, 'nomain');
assert.strictEqual(bar('none', null, { noMain: true, unpaired: true }).state, 'nomain');
assert.strictEqual(bar('ready', null, { linkLost: true }).state, 'nolink', 'no word of a main board either way: No contact, as before');

// When it is said (mainBoardAway(), linkLost()), on the page's own functions with a clock.
{
  const w = { lastConfigOkMs: 0, sidekickAnswerMs: 0, mainBoardLinked: null, pageLoadedMs: 1000, LINK_LOST_MS: 30000, now: 1000 };
  vm.createContext(w);
  vm.runInContext('Date.now = function () { return now; };' +
    ['noteSidekickAnswer', 'mainBoardAway', 'linkLost'].map((n) => extractFunction(src, n)).join('\n'), w);
  const at = (ms) => { w.now = ms; return [w.mainBoardAway(), w.linkLost()]; };
  assert.deepStrictEqual(at(5000), [false, false], 'a page just loaded says nothing yet');
  assert.deepStrictEqual(at(31001), [false, true], 'a page that never had an answer says No contact after 30 s (it said nothing, before)');
  // The sidekick answers that it has no main board: known from the first reply, with no good answer ever seen.
  w.now = 6000; w.noteSidekickAnswer({ ok: false, host_linked: '0' });
  assert.deepStrictEqual(at(6001), [true, false], 'Main board not connected, at once');
  assert.deepStrictEqual(at(16000), [true, false], 'and still, between replies that take the sidekick 5 s each');
  assert.deepStrictEqual(at(36001), [false, true], 'the sidekick went quiet too: No contact');
  // Plugged in: the next reply clears it.
  w.now = 40000; w.noteSidekickAnswer({ host_linked: '1' }); w.lastConfigOkMs = 40000;
  assert.deepStrictEqual(at(40001), [false, false], 'cleared by the reply that says a main board is linked');
  // A main board that is linked and stops answering config is not this state.
  w.now = 60000; w.noteSidekickAnswer({ ok: false, host_linked: '1' });
  assert.deepStrictEqual(at(71000), [false, true], 'linked but not answering for 30 s: No contact');
  // A sidekick from before it said (no host_linked) changes nothing.
  const o = { lastConfigOkMs: 0, sidekickAnswerMs: 0, mainBoardLinked: null, pageLoadedMs: 0, LINK_LOST_MS: 30000, now: 0 };
  vm.createContext(o);
  vm.runInContext('Date.now = function () { return now; };' +
    ['noteSidekickAnswer', 'mainBoardAway', 'linkLost'].map((n) => extractFunction(src, n)).join('\n'), o);
  o.now = 1000; o.noteSidekickAnswer({ ok: false });
  assert.strictEqual(o.mainBoardAway(), false);
}


// 2. A transmission that did not go outranks a radio problem, and offers no Stop.
s = bar('lost', tx({ aborted: true, reason: 'SWR protection' }), { swrTrips: 1 });
assert.strictEqual(s.state, 'trip');
assert.strictEqual(s.tone, 'red');
assert.strictEqual(s.sub, 'Stopped, not sent · K9ABC tries next slot');
assert.strictEqual(s.stop, '');
s = bar('ready', tx({ aborted: true, reason: 'SWR protection' }), { swrTrips: 2 });
assert.strictEqual(s.state, 'trip2');
assert.strictEqual(s.sub, 'CQ ended · queue cleared · check the antenna');
assert.strictEqual(bar('ready', tx({ nk: true, reason: 'SWR protection' })).state, 'trip2',
  'a slot skipped for SWR protection is the stopped state');
assert.strictEqual(bar('ready', null, { swrTrips: 2 }).state, 'trip2', 'stopped stays said until the next transmission');
assert.strictEqual(bar('ready', null, { swrTrips: 1 }).state, 'ready', 'one trip is over when its slot is');
s = bar('lost', tx({ nk: true, reason: 'CAT not ready' }));
assert.strictEqual(s.state, 'notkeyed');
assert.strictEqual(s.tone, 'amber');
assert(s.sub.indexOf('Not counted: CAT not ready') === 0);
assert.strictEqual(s.stop, '', 'not keyed: nothing on the air to stop');
assert.strictEqual(bar('ready', tx({ aborted: true, reason: 'low battery' })).state, 'aborted');

// noteTxHud counts tripped transmissions in a row, once per transmission, and a clean one resets.
const t = { count: 0, cur: null };
const feed = (...evs) => evs.forEach((e) => sb.noteTxHud(t, e));
const on = { active: true }, trip = { active: true, aborted: true, reason: 'SWR protection' }, off = { active: false };
feed(on, trip, trip, trip, off);
assert.strictEqual(t.count, 1, 'repeated HUD events of one tripped transmission count once');
feed(on, trip, off);
assert.strictEqual(t.count, 2, 'a second tripped transmission in a row');
feed(on, on, off);
assert.strictEqual(t.count, 0, 'a clean transmission resets the count');
feed(on, trip, off, { active: true, nk: true, reason: 'SWR protection' }, off);
assert.strictEqual(t.count, 1, 'a skipped slot is not a transmission');

assert.strictEqual(sb.utcDayLeft(Date.UTC(2026, 9, 25, 20, 48, 0)), '3h12m');
assert.strictEqual(sb.utcDayLeft(Date.UTC(2026, 9, 25, 23, 59, 30)), '0h01m');

console.log('PASS: radio bar');
