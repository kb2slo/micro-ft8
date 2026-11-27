#!/usr/bin/env node
// What has gone to QRZ is the station's (docs/architecture/webapp.md, QRZ sync), in two browsers on one
// simulated station: A, paired, sends the station's QSO to the stand-in relay, which gives it an ID, and the
// station marks the record with it; B, never paired, opens the QSOs tab and reads Up to date from the station
// alone, with no call to QRZ.
'use strict';
const { run, check, until } = require('./harness');

const CALL = 'AG6AQ';  // host_mock/test_qso.json's MY_CALLSIGN
const KEY = 'WXYZ-1234-5678-ABCD';

run(async (st) => {
  const unsent = async () => (await (await fetch(st.origin + '/api/log/entries?unsent=1&take=1')).json());
  await st.put('qrz_key=' + KEY + '\nqrz_book=' + CALL);
  const logged = await until(async () => (await unsent()).total >= 1, 60000);
  check('the simulator logs a QSO, unsent', logged, await unsent());

  let ids = 0;
  const relay = (p, body) => {
    if (p === '/qrz/status') return { ok: true, callsign: CALL, count: 0 };
    if (p === '/qrz/fetch') return { ok: true, records: [] };
    if (p === '/qrz/insert') return { ok: true, results: (body.records || []).map(() => ({ result: 'inserted', logid: String(5550000 + ++ids) })) };
    return { ok: false, error: 'qrz' };
  };
  const A = await st.browser('A', { relay });
  await A.evaluate(() => { showView('log'); });
  await until(() => A.evaluate(() => qrz.phase === 'idle' && !!qrz.result), 20000);
  await A.evaluate(() => qrzRun(true, true));
  const sent = await until(async () => A.relayCalls.some((r) => r.path === '/qrz/insert') && await A.evaluate(() => qrz.phase === 'idle'), 20000);
  check('A sends the QSO to QRZ', sent, A.relayCalls.map((r) => r.path));
  const after = await unsent();
  check('the station marks it: nothing unsent', after.total === 0 && after.entries.length === 0, after);
  check('A reads Up to date', (await A.textContent('#qrzRowText')) === 'Up to date', await A.textContent('#qrzRowText'));

  const B = await st.browser('B', { relay, paired: false });
  await B.evaluate(() => { showView('log'); });
  const upToDate = await until(async () => (await B.textContent('#qrzRowText')) === 'Up to date', 20000);
  check('B, never paired, reads Up to date', upToDate, await B.textContent('#qrzRowText'));
  check('with no call to QRZ', B.relayCalls.length === 0, B.relayCalls.map((r) => r.path));
  check('and no tap on its row', (await B.getAttribute('#qrzRow', 'aria-disabled')) === 'true');
});
