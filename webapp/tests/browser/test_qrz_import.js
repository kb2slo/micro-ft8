#!/usr/bin/env node
// Import from QRZ (docs/architecture/webapp.md, QRZ sync), in a paired browser on one simulated station, with
// QRZ stood in for: a logbook holding the station's own QSO, two it never logged (FT8 and FT4) and one in SSB.
// The import matches the station's QSO first and so updates it rather than adding a copy, brings the two in
// marked QRZ, leaves the SSB one out and says so; their cards and the QSOs list show them, and a second
// import adds nothing.
'use strict';
const { run, check, until } = require('./harness');

const CALL = 'AG6AQ';  // host_mock/test_qso.json's MY_CALLSIGN
const PARTNER = 'N6HAN';

run(async (st) => {
  const page = async (q) => (await fetch(st.origin + '/api/log/entries?take=10' + (q || ''))).json();
  await st.put('qrz_key=WXYZ-1234-5678-ABCD\nqrz_book=' + CALL);
  check('the simulator logs its QSO', await until(async () => (await page()).total >= 1, 60000));
  const ours = (await page()).entries[0];

  const book = [
    { app_qrzlog_logid: '7001', call: PARTNER, qso_date: ours.date, time_on: ours.time.replace(':', ''), band: '20m', mode: 'FT8',
      station_callsign: CALL, dxcc: '291', state: 'CA', app_qrzlog_status: 'C' },
    { app_qrzlog_logid: '7002', call: 'DL1AA', qso_date: '20240301', time_on: '1200', band: '20m', mode: 'FT8',
      station_callsign: CALL, gridsquare: 'JO62', freq: '14.074', rst_sent: '-05', rst_rcvd: '-11', dxcc: '230' },
    { app_qrzlog_logid: '7003', call: 'G4ABC', qso_date: '20240302', time_on: '1300', band: '20m', mode: 'SSB', station_callsign: CALL },
    { app_qrzlog_logid: '7004', call: 'VK2XYZ', qso_date: '20250615', time_on: '0815', band: '40m', mode: 'MFSK', submode: 'FT4',
      station_callsign: CALL, gridsquare: 'QF56' },
  ];
  const relay = (p, body) => {
    if (p === '/qrz/status') return { ok: true, callsign: CALL, count: book.length };
    if (p === '/qrz/fetch') {
      const from = body.from.replace(/-/g, ''), to = body.to.replace(/-/g, '');
      const records = book.filter((r) => r.qso_date >= from && r.qso_date <= to && Number(r.app_qrzlog_logid) >= body.after);
      return { ok: true, records, more: false, next: 9999 };
    }
    if (p === '/qrz/insert') return { ok: true, results: (body.records || []).map(() => ({ result: 'duplicate' })) };
    return { ok: false, error: 'qrz' };
  };
  const A = await st.browser('A', { relay });
  await A.settings('station');
  check('the button is there with a key', await until(() => A.isEnabled('#qrzImport'), 5000));
  await A.click('#qrzImport');
  const done = await until(async () => /imported/.test((await A.textContent('#qrzImportLine')) || ''), 30000);
  const line = await A.textContent('#qrzImportLine');
  check('the import says how it went', done && line === '3 imported · 1 not FT8 or FT4', line);

  const all = await page(), own = await page('&own=1');
  check('two QSOs came in, the station\'s own not twice', all.total === 3 && own.total === 1, [all.total, own.total]);
  const imported = all.entries.filter((q) => q.imported).map((q) => q.call).sort();
  check('marked imported', JSON.stringify(imported) === '["DL1AA","VK2XYZ"]', imported);
  const vk = all.entries.find((q) => q.call === 'VK2XYZ');
  check('FT4 as the station logs it', vk && vk.mode === 'FT4' && vk.band === '40m' && vk.grid === 'QF56', vk);
  check('nothing in SSB', !all.entries.some((q) => q.call === 'G4ABC'));

  await A.evaluate((c) => showCallInfo(c), 'DL1AA');
  const card = await until(async () => /QSOs in log \(1\)/.test(await A.textContent('#ciQsos')), 10000);
  check('a station worked only from QRZ shows on its card', card && /2024-03-01 12:00 · 20m FT8/.test(await A.textContent('#ciQsos')),
    await A.textContent('#ciQsos'));
  await A.evaluate(() => CallInfo.hide());

  await A.evaluate(() => showView('log'));
  const tagged = await until(async () => (await A.$$('#logList .qtag')).length === 2, 10000);
  check('the QSOs list shows them, tagged QRZ', tagged, (await A.$$('#logList .qtag')).length);
  check('and its line counts them', /^3 QSOs · /.test(await A.textContent('#logSummary')), await A.textContent('#logSummary'));

  await A.settings('station');
  await A.click('#qrzImport');
  await until(async () => /imported/.test((await A.textContent('#qrzImportLine')) || '') && !(await A.evaluate(() => qrzImporting)), 30000);
  check('a second import adds nothing', (await page()).total === 3, (await page()).total);
});
