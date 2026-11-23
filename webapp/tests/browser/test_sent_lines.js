#!/usr/bin/env node
// What we sent, in the decodes (docs/architecture/webapp.md, What we sent), end to end in two browsers on one
// simulated station: as the scenario's QSO runs, each page shows our transmissions as lines among the decode
// slots, from the station's own TX HUD events, Sending while on the air and Sent once ended; our answer to
// N6HAN's CQ sits above the slot that decoded the CQ (the simulator sends it in that slot, decodeListOrder());
// and a reload keeps the lines, which the station does not replay. node webapp/tests/test_sent_lines.js holds the rules; this holds the wiring.
//
// At real time (--scale 1): a line is its slot by the station's time on the event, and the simulator stamps
// events with the wall clock, so a faster run puts several of its periods' transmissions in one slot, which a
// station never does. About 30 s.
'use strict';
const { run, check, until } = require('./harness');

const FIRST = 'N6HAN AG6AQ CM97';  // host_mock/test_qso.json: our answer to N6HAN's CQ

// The decode list as the operator reads it, top to bottom: a slot's header, or a line of what we sent.
const listed = (p) => p.evaluate(() => Array.from(document.querySelectorAll('#opDecodeRows > .dslot')).map((el) =>
  el.classList.contains('dsent')
    ? 'sent:' + el.querySelector('.sentlbl').textContent + ':' + el.querySelector('.senttxt').textContent
    : 'slot:' + Array.from(el.querySelectorAll('.drow .txt')).map((t) => t.textContent).join('|')));

run(async (st) => {
  const A = await st.browser('A');
  const B = await st.browser('B');
  await A.evaluate(() => showView('operate'));
  await B.evaluate(() => showView('operate'));

  const sent = await until(async () => {
    const l = await listed(A);
    return l.some((x) => x === 'sent:Sent:' + FIRST) ? l : null;
  }, 75000);
  check('A shows our answer as a Sent line', !!sent, await listed(A));
  if (!sent) return;

  const i = sent.indexOf('sent:Sent:' + FIRST);
  const cq = sent.findIndex((x) => /^slot:.*CQ N6HAN CM87/.test(x));
  check('the answer sits above the slot that decoded the CQ it answers', cq > i, sent);

  const lb = await until(async () => {
    const l = await listed(B);
    return l.indexOf('sent:Sent:' + FIRST) !== -1 ? l : null;
  }, 20000);
  check('B, from the same events, shows the same line', !!lb, await listed(B));

  const seen = await until(async () => {
    const l = await listed(A);
    return l.some((x) => /^sent:Sending:/.test(x)) ? l : null;
  }, 40000);
  check('a transmission on the air reads Sending', !!seen, await listed(A));

  // The station replays no history of what we sent: the line after a reload is the page's own record.
  await A.evaluate(() => { saveStateCache(); location.reload(); });
  await A.waitForFunction(() => typeof lastCfg === 'object' && lastCfg.call !== undefined, null, { timeout: 15000 });
  await A.evaluate(() => showView('operate'));
  const kept = await until(async () => {
    const l = await listed(A);
    return l.some((x) => /^sent:(Sent|Sending):/.test(x)) ? l : null;
  }, 10000);
  check('after a reload the lines are still there', !!kept, await listed(A));
}, { scale: 1 });
