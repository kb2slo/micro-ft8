#!/usr/bin/env node
// The callsign card's heard section reads the station's heard ring (row c5r3; docs/architecture/webapp.md, the
// callsign card), in a browser on one simulated station: once the simulator has decoded its partner, the card
// for that call lists those decodes newest first with a line and a trend for the band and mode, a page at a time;
// a page opened after the decodes went by still shows them; and a call never heard says none.
'use strict';
const { run, check, until } = require('./harness');

const PARTNER = 'N6HAN';  // host_mock/test_qso.json: the station the simulator decodes and works

run(async (st) => {
  const heard = async () => (await (await fetch(st.origin + '/api/heard?take=1&call=' + PARTNER)).json()).total;
  check('the simulator decodes its partner', await until(async () => (await heard()) >= 3, 60000), await heard());

  // Opened after the decodes went by: nothing of them is in this page's own decode list's past.
  const A = await st.browser('A');
  await A.evaluate((c) => showCallInfo(c), PARTNER);
  const read = await until(async () => /Heard \(\d+\) · since \d{2}:\d{2}z/.test(await A.textContent('#ciHeard')), 10000);
  const card = await A.textContent('#ciHeard');
  check('the card reads what the station heard of the call', read, card);
  check('with a line for the band and mode', /20m FT8 ×\d+ · [^·]+ · [-+]?\d+ dB now, median [-+]?\d+, best [-+]?\d+/.test(card), card);
  // The trend needs two hours with decodes, which a run of a few minutes never has (test_callinfo_heard.js draws it).
  check('no trend from one hour', !(await A.$('#ciHeard svg.ci-spark')));
  check('and the decodes themselves', /\d{2}:\d{2}:\d{2}z · 20m FT8 · [-+]?\d+ dB · \d+ Hz · .*N6HAN/.test(card), card);

  const total = await heard();
  const rows = async () => A.evaluate(() => document.querySelectorAll('#ciHeardRows .ci-heard').length);
  if (total > 10) {
    check('a page of ten', (await rows()) === 10, await rows());
    await A.click('#ciHeardMore');
    check('Show more reads the next page from the last row', await until(async () => (await rows()) === Math.min(total, 20), 10000), await rows());
  } else {
    check('every decode on one page', (await rows()) === total, { rows: await rows(), total });
    check('no Show more', !(await A.$('#ciHeardMore')));
  }
  const texts = await A.evaluate(() => Array.from(document.querySelectorAll('#ciHeardRows .ci-heard')).map((e) => e.textContent));
  const times = texts.map((t) => t.slice(0, 9));
  check('newest first', times.every((t, i) => i === 0 || times[i - 1] >= t), times);

  await A.evaluate(() => CallInfo.hide());
  await A.evaluate(() => showCallInfo('DL1AA'));
  const none = await until(async () => /No decodes from this callsign/.test(await A.textContent('#ciHeard')), 10000);
  check('a call never heard: none', none, await A.textContent('#ciHeard'));
});
