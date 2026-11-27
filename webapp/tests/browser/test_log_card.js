#!/usr/bin/env node
// The callsign card's QSOs come from the station's whole log, and QSOs opens on a line summing it up
// (docs/architecture/webapp.md, QSOs and the callsign card), in a browser on one simulated station: once the
// simulator logs its QSO, the card for that call says so before the QSOs list has loaded anything, and a call
// never worked says none.
'use strict';
const { run, check, until } = require('./harness');

const PARTNER = 'N6HAN';  // host_mock/test_qso.json: the QSO the simulator completes and logs

run(async (st) => {
  const total = async () => (await (await fetch(st.origin + '/api/log/entries?take=1')).json()).total;
  check('the simulator logs its QSO', await until(async () => (await total()) >= 1, 60000), await total());

  const A = await st.browser('A');
  check('the QSOs list has not loaded', await A.evaluate(() => !logLoadedOnce));
  await A.evaluate((c) => showCallInfo(c), PARTNER);
  const read = await until(async () => /QSOs in log \(1\)/.test(await A.textContent('#ciQsos')), 10000);
  const card = await A.textContent('#ciQsos');
  check('the card reads the call\'s QSO from the station', read, card);
  check('with its summary', /20m FT8 ×1/.test(card), card);
  check('and the QSO itself', /\d{4}-\d{2}-\d{2} \d{2}:\d{2} · 20m FT8 · 14\.074 MHz/.test(card), card);
  check('no Show more for one QSO', !(await A.$('#ciQsoMore')));

  await A.evaluate(() => CallInfo.hide());
  await A.evaluate(() => showCallInfo('DL1AA'));
  const none = await until(async () => /No QSOs with this callsign in the log/.test(await A.textContent('#ciQsos')), 10000);
  check('a call never worked: none', none, await A.textContent('#ciQsos'));
  await A.evaluate(() => CallInfo.hide());

  await A.evaluate(() => showView('log'));
  const line = await until(async () => /^1 QSO · \w{3} \d+, \d{4} · 1 day on the air$/.test((await A.textContent('#logSummary')) || ''), 10000);
  check('QSOs opens on the summary line', line, await A.textContent('#logSummary'));
});
