#!/usr/bin/env node
// Settings that live on the station (docs/architecture/webapp.md, Settings that live on the station), end to
// end in three browsers on one simulated station: A and B paired, C never. The display settings and the CQ
// row's mode set in one show in the others; theme stays in its browser; a browser that is not paired cannot
// change them and is put back; the QRZ key is masked on every open config read, read whole only with the
// token, and sent whole to the relay, which is stood in for, by a paired browser alone.
'use strict';
const { run, check, until, TOKEN } = require('./harness');

const KEY1 = 'WXYZ-1234-5678-ABCD';
const KEY2 = 'QQQQ-RRRR-SSSS-9876';
const BAD = 'BAD0-BAD0-BAD0-BAD0';
const CALL = 'AG6AQ';  // host_mock/test_qso.json's MY_CALLSIGN: the logbook the stand-in relay names

run(async (st) => {
  let status = { ok: true, callsign: CALL, count: 0 };
  const relay = (p, body) => {
    if (p === '/qrz/status') return status;
    if (p === '/qrz/fetch') return { ok: true, records: [] };
    if (p === '/qrz/insert') return { ok: true, results: (body.records || []).map(() => ({ result: 'inserted' })) };
    return { ok: false, error: 'qrz' };
  };
  const blur = (p) => p.evaluate(() => document.getElementById('qrzKey').blur());

  // A fresh station's defaults.
  const A = await st.browser('A', { relay });
  await A.settings('device');
  check('defaults: km, bearing off, queue marks on', (await A.inputValue('#dist_unit')) === 'km'
    && !(await A.isChecked('#show_bearing')) && (await A.isChecked('#show_queue_marks')));
  check('the page is headed Display', (await A.textContent('#setp-device h2')) === 'Display');

  // A changes the three; the station keeps them. Theme stays in A.
  await A.selectOption('#dist_unit', 'mi');
  await A.click('label:has(#show_bearing)');
  await A.click('label:has(#show_queue_marks)');
  const kept = await until(async () => {
    const c = await st.station();
    return c.dist_unit === 'mi' && c.show_bearing === '1' && c.queue_marks === '0';
  }, 5000);
  check('the station holds mi, bearing on, marks off', kept, await st.station());
  await A.selectOption('#theme_pref', 'dark');
  check('theme is not sent to the station', (await st.station()).theme === undefined);

  // A second browser shows the station's, and keeps its own theme.
  // B's config reads and relay are slow, as a busy main board and QRZ can be, so a run started by opening the
  // QSOs tab is still under way when the switch asks for one (CI, 2026-11-22: that run was dropped, and
  // sending by itself never sent, leaving "1 to send").
  const B = await st.browser('B', { relay, slowConfig: 1500, relayDelay: 1500 });
  await B.settings('device');
  check('B shows mi, bearing on, marks off', (await B.inputValue('#dist_unit')) === 'mi'
    && (await B.isChecked('#show_bearing')) && !(await B.isChecked('#show_queue_marks')));
  check('B keeps its own theme', (await B.evaluate(() => document.documentElement.getAttribute('data-theme'))) === null
    && (await A.evaluate(() => document.documentElement.getAttribute('data-theme'))) === 'dark');
  await B.evaluate(() => showView('operate'));
  const rows = await until(async () => {
    const t = await B.textContent('#opDecodes');
    return / mi\b/.test(t) && /\d{3}°/.test(t);
  }, 40000);
  check('B\'s decode rows show miles and the bearing', rows, (await B.textContent('#opDecodes')).slice(0, 200));
  check('fmtKm() and callinfo.js use miles', await B.evaluate(() => fmtKm(100) === '62 mi'));

  // The map reads the station's unit.
  const map = await B.context().newPage();
  await map.goto(st.origin + '/app/map.html');
  const mapMi = await until(() => map.evaluate(() => typeof distUnit !== 'undefined' && distUnit === 'mi' && fmtKm(100) === '62 mi'), 8000);
  check('map.html uses miles', mapMi);
  await map.close();

  // A browser that is not paired cannot change them, and is put back.
  const C = await st.browser('C', { relay, paired: false });
  check('C is not paired', !(await C.evaluate(() => !!MicroFT8Pairing.getToken())));
  await C.settings('device');
  check('C shows the station\'s miles', (await C.inputValue('#dist_unit')) === 'mi');
  await C.selectOption('#dist_unit', 'km');
  const told = await until(async () => (await C.toasts()).some((t) => /pair/i.test(t)), 3000);
  check('C\'s change is refused, with a toast', told, await C.toasts());
  check('C\'s control is back to miles', (await C.inputValue('#dist_unit')) === 'mi' && await C.evaluate(() => distUnit === 'mi'));
  check('the station still says mi', (await st.station()).dist_unit === 'mi');

  // The CQ row's mode is the station's.
  await st.put('listen5=3');  // band_sel 5 listens to FT8 and FT4, so there are two to choose from
  await A.evaluate(() => { showView('operate'); return loadConfig(); });
  await B.evaluate(() => { showView('operate'); return loadConfig(); });
  check('the mode button shows with both channels', await A.isVisible('#cqModeBtn'));
  const before = (await A.textContent('#cqModeBtn')).trim();
  await A.click('#cqModeBtn');
  const ft4 = await until(async () => (await st.station()).cq_mode === 'FT4', 5000);
  check('a tap on A sets cq_mode=FT4 on the station', ft4 && /^FT8/.test(before), before);
  const follows = await until(async () => /^FT4/.test((await B.textContent('#cqModeBtn')).trim()), 15000);
  check('B\'s button follows by its own poll', follows, (await B.textContent('#cqModeBtn')).trim());

  // The QRZ key, typed on A: kept on the station, masked on the open read.
  await A.settings('station');
  await A.fill('#qrzKey', KEY1);
  await blur(A);
  const named = await until(async () => (await A.textContent('#qrzKeyLine')).includes(CALL + ' logbook'), 8000);
  check('A\'s line names the logbook', named, await A.textContent('#qrzKeyLine'));
  check('A\'s field shows the key masked', (await A.inputValue('#qrzKey')) === 'xxxxxxxxxxxxxxxABCD', await A.inputValue('#qrzKey'));
  const s1 = await st.station();
  check('the open read: the key masked, its logbook, sending off',
    s1.qrz_key === 'xxxxxxxxxxxxxxxABCD' && s1.qrz_book === CALL && s1.qrz_live === '', s1);
  check('GET /api/qrz_key without the token: 401', (await fetch(st.origin + '/api/qrz_key')).status === 401);
  check('the status call carried the key as typed', A.relayCalls.some((r) => r.path === '/qrz/status' && r.body.key === KEY1));

  // B sees it, turns sending by itself on, and sends with the key whole.
  await B.evaluate(() => loadConfig());
  await B.settings('station');
  check('B shows the masked key and its logbook', (await B.inputValue('#qrzKey')) === 'xxxxxxxxxxxxxxxABCD'
    && (await B.textContent('#qrzKeyLine')).includes(CALL + ' logbook'));
  await B.selectOption('#qrzLive', '1');
  const day = await until(async () => /^\d{8}$/.test((await st.station()).qrz_live), 5000);
  check('the station holds the day sending was turned on', day, (await st.station()).qrz_live);
  await B.evaluate(() => showView('log'));
  const sent = await until(async () => B.relayCalls.some((r) => r.path === '/qrz/insert'), 60000);
  check('B sent a QSO to QRZ by itself', sent, B.relayCalls.map((r) => r.path));
  check('every call B made carried the key whole', B.relayCalls.length > 0 && B.relayCalls.every((r) => r.body.key === KEY1),
    B.relayCalls.map((r) => r.body.key));
  await until(() => B.evaluate(() => qrz.phase === 'idle'), 5000);
  // Up to date, or a QSO the simulator logged since, waiting out the gap between runs: neither is a problem.
  check('B\'s row is up to date or waiting, not amber', /Up to date|waiting/.test(await B.textContent('#qrzRowText'))
    && !(await B.evaluate(() => document.getElementById('qrzRow').classList.contains('t-amber'))), await B.textContent('#qrzRowText'));
  await A.evaluate(() => loadConfig());
  check('A\'s switch reads On', (await A.inputValue('#qrzLive')) === '1');

  // The browser that is not paired cannot reach QRZ. With nothing unsent it would read Up to date from the
  // station alone (test_qrz_marks.js), so one QSO is marked unsent again, as a write-back that never reached
  // the station leaves it.
  const unmark = () => fetch(st.origin + '/api/log/entries?id=1', { method: 'PATCH', headers: { 'X-MicroFT8-Token': TOKEN },
    body: '<app_qrzlog_logid:0><qrzcom_qso_upload_status:0>' });
  await unmark();
  await C.evaluate(() => { qrz.result = null; showView('log'); return loadConfig().catch(() => {}); });
  await until(() => C.evaluate(() => qrz.phase === 'idle'), 10000);
  await C.evaluate(() => qrzRun(false, true));
  check('C\'s row asks to pair', (await C.textContent('#qrzRowText')) === 'Pair this browser to reach QRZ', await C.textContent('#qrzRowText'));
  check('C made no relay call', C.relayCalls.length === 0, C.relayCalls);
  await C.click('#qrzRow');
  check('a tap on it opens Pairing', await until(() => C.isVisible('#setp-pairing'), 2000));

  // B changes the key: sending by itself goes off again, and A follows and reads the new one whole.
  await B.settings('station');
  await B.fill('#qrzKey', KEY2);
  await blur(B);
  await until(async () => (await st.station()).qrz_key === 'xxxxxxxxxxxxxxx9876', 8000);
  const s2 = await st.station();
  check('the new key replaces the old, sending off', s2.qrz_key === 'xxxxxxxxxxxxxxx9876' && s2.qrz_live === '', s2);
  await A.evaluate(() => loadConfig());
  check('A\'s field shows the new mask', (await A.inputValue('#qrzKey')) === 'xxxxxxxxxxxxxxx9876', await A.inputValue('#qrzKey'));
  // A run asks QRZ only for what the station holds unsent.
  await unmark();
  const n = A.relayCalls.length;
  await A.evaluate(() => { qrz.result = null; return qrzRun(false, true); });
  const fresh = A.relayCalls.slice(n);
  check('A\'s next run uses the new key whole', fresh.length > 0 && fresh.every((r) => r.body.key === KEY2), fresh.map((r) => r.body.key));

  // A key QRZ refuses is still kept, and says so.
  status = { ok: false, error: 'auth' };
  await B.fill('#qrzKey', BAD);
  await blur(B);
  await until(async () => (await B.textContent('#qrzKeyLine')).includes('refused'), 8000);
  check('the line says QRZ refused it', (await B.textContent('#qrzKeyLine')) === 'QRZ refused this key');
  check('the row says so', (await B.textContent('#qrzRowText')) === 'QRZ refused the key');
  const s3 = await st.station();
  check('the station keeps it, with no logbook', s3.qrz_key === 'xxxxxxxxxxxxxxxBAD0' && s3.qrz_book === '', s3);
  status = { ok: true, callsign: CALL, count: 0 };

  // Clearing it clears all three.
  await B.fill('#qrzKey', '');
  await blur(B);
  await until(async () => (await st.station()).qrz_key === '', 5000);
  const s4 = await st.station();
  check('cleared: no key, logbook or day', s4.qrz_key === '' && s4.qrz_book === '' && s4.qrz_live === '', s4);
  check('B\'s row reads Not set up', await until(async () => (await B.textContent('#qrzRowText')) === 'Not set up', 5000),
    await B.textContent('#qrzRowText'));
  check('B\'s field is empty', (await B.inputValue('#qrzKey')) === '');

  // No open read ever carried a key whole.
  const reads = [A, B, C].flatMap((p) => p.configReads);
  const leaks = reads.filter((t) => [KEY1, KEY2, BAD].some((k) => t.includes(k)));
  check(reads.length + ' open config reads, none with a key whole', reads.length > 10 && leaks.length === 0, leaks.length);
});
