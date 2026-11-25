#!/usr/bin/env node
// What we sent, in the decodes (docs/architecture/webapp.md, What we sent), end to end in two browsers on one
// simulated station: as the scenario's QSO runs, each page shows our transmissions as lines among the decode
// slots, from the station's own TX HUD events, Sending while on the air and Sent once ended; our answer to
// N6HAN's CQ sits above the slot that decoded the CQ (the simulator sends it in that slot, decodeListOrder());
// a reload keeps the lines, which the station does not replay; and a line from long before, with no decode
// slots kept, is not drawn. node webapp/tests/test_sent_lines.js holds the rules; this holds the wiring.
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

  // Its age, as a decode slot's header shows one, counts up on the page's tick with no event (refreshVisibleAgo()).
  // Read from the oldest line that reads Sent: its slot has ended, so it counts rather than reading "just now".
  const ago = () => A.evaluate(() => {
    const el = Array.from(document.querySelectorAll('#opDecodeRows .dsent'))
      .filter((d) => d.querySelector('.sentlbl').textContent === 'Sent').pop();
    const a = el && el.querySelector('.senttime .ago[data-ts]');
    return a ? a.textContent : null;
  });
  const ago0 = await until(async () => { const a = await ago(); return /^\d+s ago$/.test(a || '') ? a : null; }, 20000);
  const ago1 = ago0 && await until(async () => { const a = await ago(); return a && a !== ago0 ? a : null; }, 3000);
  check('the Sent line shows its age, and it counts up with no event', !!ago0 && /ago$/.test(ago1 || ''), [ago0, ago1]);

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

  // A station gone quiet keeps no decode slots: a line from long before is not drawn with none around it.
  const quiet = await B.evaluate(() => {
    const line = (x, agoMs) => ({ x: x, hz: 1000, md: 'FT8', ts: Date.now() - agoMs, lenMs: 15000, st: 'sent', reason: '' });
    decodeSlots = [];
    sentTx = [line('CQ OLD EM10', 600000), line('CQ NEW EM10', 15000)];
    renderDecodes(true);
    return Array.from(document.querySelectorAll('#opDecodeRows .senttxt')).map((el) => el.textContent);
  });
  check('with no decode slots kept, a recent line shows and one from ten minutes ago does not',
    quiet.join() === 'CQ NEW EM10', quiet);
}, { scale: 1 });
