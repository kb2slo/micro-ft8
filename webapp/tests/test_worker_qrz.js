#!/usr/bin/env node
// worker.js's /qrz/ route (docs/architecture/system.md, Third-party services): what it asks of QRZ's
// Logbook API, what it passes back, and what it refuses before asking. QRZ is a stand-in here, answering
// in the shapes its real replies had on 2026-11-19. Not Cloudflare's own behaviour, and not QRZ's.
'use strict';
const assert = require('assert');
const path = require('path');
const { pathToFileURL } = require('url');

const KEY = 'ABCD-1234-EF56-7890';
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

// Two records as QRZ sends them: a field a line, the other operator's details among them. The first has
// a name with an accent, whose length QRZ counts in characters, and a comment holding an <eor> of its own.
const FETCHED = esc([
  '<my_gridsquare:6>EM10CG', '<call:5>N5XYZ', '<name:11>JOSÉ GARCÍA', '<email:15>jose@example.org', '<qth:9>KETTERING',
  '<lat:11>N039 42.077', '<band:3>20m', '<comment:12>5W <eor> ant', '<time_off:4>2355', '<freq:7>14.0755',
  '<mode:3>FT8', '<station_callsign:5>K1ABC', '<app_qrzlog_logid:10>1458502669', '<qso_date:8>20260528',
  '<app_qrzlog_status:1>N', '<time_on:4>2354', '<qrzcom_qso_upload_status:1>Y', '<dxcc:3>291',
  '<eor>', '',
  '<mode:4>MFSK', '<submode:3>FT4', '<band:3>20M', '<call:6>VA3ABC', '<station_callsign:5>K1ABC',
  '<app_qrzlog_logid:10>1531992047', '<qso_date:8>20261115', '<time_on:4>2210', '<app_qrzlog_status:1>C',
  '<app_qrzlog_qsldate:8>20261120', '<name:9>SOMEBODY', '<eor>', '',
].join('\n'));

const calls = [];
let answers = [];
globalThis.fetch = async (url, init) => {
  const form = new URLSearchParams(init.body);
  calls.push({ url, agent: init.headers['user-agent'], method: init.method, form: Object.fromEntries(form) });
  const next = answers.shift();
  if (next instanceof Error) throw next;
  return new Response(next);
};

(async function () {
  const mod = await import(pathToFileURL(path.join(__dirname, '..', 'worker.js')).href);
  const worker = mod.default;
  const post = async (p, body, method) => {
    const r = await worker.fetch(new Request('https://micro-ft8.kalikstein.com' + p, {
      method: method || 'POST', body: method === 'GET' ? undefined : (typeof body === 'string' ? body : JSON.stringify(body)),
    }), {});
    return { status: r.status, headers: r.headers, text: await r.text() };
  };
  const ask = async (p, body) => {
    const r = await post(p, body);
    assert.ok(!r.text.includes(KEY), 'no reply repeats the key: ' + r.text);
    assert.strictEqual(r.headers.get('access-control-allow-origin'), '*', 'the sidekick\'s page may read it');
    assert.strictEqual(r.headers.get('cache-control'), 'no-store');
    return Object.assign(JSON.parse(r.text), { http: r.status });
  };

  // QRZ's replies are NAME=value pairs in no fixed order, the ADIF escaped.
  assert.deepStrictEqual(mod.parseQrzReply('COUNT=2&RESULT=OK&ADIF=&lt;call:3&gt;A&amp;B\n&lt;eor&gt;'),
    { COUNT: '2', RESULT: 'OK', ADIF: '&lt;call:3&gt;A&amp;B\n&lt;eor&gt;' });
  assert.deepStrictEqual(mod.parseQrzReply('STATUS=AUTH&RESULT=AUTH&REASON=invalid api key\n&EXTENDED='),
    { STATUS: 'AUTH', RESULT: 'AUTH', REASON: 'invalid api key', EXTENDED: '' });

  // Status: the logbook's callsign and totals.
  answers = ['END_DATE=2036-07-19&ACTION=STATUS&BOOKID=470330&CONFIRMED=874&CALLSIGN=K1ABC&DXCC_COUNT=46&' +
    'BOOK_NAME=K1ABC Logbook&RESULT=OK&COUNT=1302&OWNER=K1ABC&START_DATE=2026-05-28'];
  assert.deepStrictEqual(await ask('/qrz/status', { key: KEY }),
    { ok: true, callsign: 'K1ABC', count: 1302, confirmed: 874, dxcc_count: 46, start_date: '2026-05-28', http: 200 });
  assert.deepStrictEqual(calls[0], { url: 'https://logbook.qrz.com/api', agent: 'micro-FT8/0 (relay 1; https://micro-ft8.kalikstein.com)',
    method: 'POST', form: { KEY: KEY, ACTION: 'STATUS' } }, 'one call to QRZ, as this application');

  // The app's version is named to QRZ when it is a plain version, and is never a way to write a header.
  assert.strictEqual(mod.qrzUserAgent('0.2.0'), 'micro-FT8/0.2.0 (relay 1; https://micro-ft8.kalikstein.com)');
  assert.strictEqual(mod.qrzUserAgent('20261119-07a08db0'), 'micro-FT8/20261119-07a08db0 (relay 1; https://micro-ft8.kalikstein.com)');
  // Every shape a build reports (docs/RELEASE_PROCESS.md): a release's and a dev build's carry a +, and a
  // bench push adds when it was built, which is dropped.
  for (const [app, version] of [['0.1.0+0c5ccec', '0.1.0+0c5ccec'], ['b1322+0c5ccec', 'b1322+0c5ccec'],
    ['0c5ccec-dirty', '0c5ccec-dirty'], ['37f14119-dirty 2026-11-23 14:57:39', '37f14119-dirty'],
    [' 1.0', '1.0'], ['1.0\r\nX-Injected: 1', '1.0']]) {
    assert.strictEqual(mod.appVersion(app), version, JSON.stringify(app));
    assert.strictEqual(mod.qrzUserAgent(app), `micro-FT8/${version} (relay 1; https://micro-ft8.kalikstein.com)`);
  }
  for (const bad of [undefined, null, 7, '', 'x'.repeat(33), '-1', '1/2', 'é1', '+1']) {
    assert.strictEqual(mod.qrzUserAgent(bad), 'micro-FT8/0 (relay 1; https://micro-ft8.kalikstein.com)', JSON.stringify(bad));
  }
  assert.ok(mod.qrzUserAgent('x'.repeat(32)).length <= 128, 'within QRZ\'s 128 characters');
  answers = ['RESULT=OK&COUNT=1&CALLSIGN=K1ABC'];
  await ask('/qrz/status', { key: KEY, app: '0.2.0' });
  assert.strictEqual(calls[calls.length - 1].agent, 'micro-FT8/0.2.0 (relay 1; https://micro-ft8.kalikstein.com)');
  assert.deepStrictEqual(calls[calls.length - 1].form, { KEY: KEY, ACTION: 'STATUS' }, 'and is no part of what QRZ is asked');

  // A key QRZ refuses.
  answers = ['STATUS=AUTH&RESULT=AUTH&REASON=invalid api key\n&EXTENDED='];
  assert.deepStrictEqual(await ask('/qrz/status', { key: KEY }), { ok: false, error: 'auth', reason: 'invalid api key', http: 200 });

  // Fetch: a date range, a page at a time, each record cut down to what a sync matches on.
  calls.length = 0;
  answers = ['COUNT=2&RESULT=OK&ADIF=' + FETCHED];
  const page = await ask('/qrz/fetch', { key: KEY, from: '2026-05-28', to: '2026-11-19', after: 0 });
  assert.deepStrictEqual(calls[0].form, { KEY: KEY, ACTION: 'FETCH', OPTION: 'BETWEEN:2026-05-28+2026-11-19,MAX:250,AFTERLOGID:0' });
  assert.deepStrictEqual(page.records, [
    { app_qrzlog_logid: '1458502669', call: 'N5XYZ', qso_date: '20260528', time_on: '2354', band: '20m', mode: 'FT8',
      station_callsign: 'K1ABC', app_qrzlog_status: 'N', freq: '14.0755', dxcc: '291' },
    { app_qrzlog_logid: '1531992047', call: 'VA3ABC', qso_date: '20261115', time_on: '2210', band: '20M', mode: 'MFSK',
      submode: 'FT4', station_callsign: 'K1ABC', app_qrzlog_status: 'C', app_qrzlog_qsldate: '20261120' },
  ], 'QRZ\'s fields under QRZ\'s names, the import\'s too, never a name, address or email; a comment\'s own <eor> ends no record, a name with an accent shifts nothing');
  assert.strictEqual(page.more, false, 'a short page is the last');
  assert.strictEqual(page.next, 1531992048, 'the next page starts after the highest ID');
  for (const personal of ['JOS', 'example.org', 'KETTERING', 'N039', 'SOMEBODY', '5W']) {
    assert.ok(!JSON.stringify(page).includes(personal), 'nobody\'s details are passed on: ' + personal);
  }

  // A full page says there may be more.
  answers = ['RESULT=OK&COUNT=250&ADIF=' + esc(Array.from({ length: 250 }, (_, i) =>
    '<call:5>W1AAA<app_qrzlog_logid:4>' + (1000 + i) + '<eor>').join('\n'))];
  const full = await ask('/qrz/fetch', { key: KEY, from: '2026-01-01', to: '2026-01-31', after: 7 });
  assert.strictEqual(calls[calls.length - 1].form.OPTION, 'BETWEEN:2026-01-01+2026-01-31,MAX:250,AFTERLOGID:7');
  assert.deepStrictEqual([full.records.length, full.more, full.next], [250, true, 1250]);

  // No record in the range is QRZ's FAIL with a count of 0, and is not an error.
  for (const empty of ['COUNT=0&RESULT=FAIL', 'RESULT=FAIL&COUNT=0']) {
    answers = [empty];
    assert.deepStrictEqual(await ask('/qrz/fetch', { key: KEY, from: '2026-01-01', to: '2026-01-02', after: 5 }),
      { ok: true, records: [], more: false, next: 5, http: 200 });
  }
  // Any other failure is one.
  answers = ['RESULT=FAIL&REASON=invalid option&COUNT=0'];
  assert.deepStrictEqual(await ask('/qrz/fetch', { key: KEY, from: '2026-01-01', to: '2026-01-02' }),
    { ok: false, error: 'qrz', reason: 'invalid option', http: 200 });

  // Insert: each record in turn. A QSO QRZ already holds is a duplicate, with no ID; anything else it
  // refuses is passed back with its reason.
  calls.length = 0;
  answers = ['LOGID=1532000001&RESULT=OK&COUNT=1',
    'STATUS=FAIL&RESULT=FAIL&REASON=Unable to add QSO to database: duplicate&EXTENDED=',
    'STATUS=FAIL&RESULT=FAIL&REASON=wrong station_callsign for this logbook&EXTENDED='];
  const sent = await ask('/qrz/insert', { key: KEY, records: ['<call:5>N5XYZ<eor>', '<call:6>VA3ABC<eor>', '<call:4>W9XY<eor>'] });
  assert.deepStrictEqual(sent, { ok: true, http: 200, results: [{ result: 'inserted', logid: '1532000001' },
    { result: 'duplicate' }, { result: 'refused', reason: 'wrong station_callsign for this logbook' }] });
  assert.deepStrictEqual(calls.map((c) => [c.form.ACTION, c.form.ADIF, c.form.OPTION]),
    [['INSERT', '<call:5>N5XYZ<eor>', undefined], ['INSERT', '<call:6>VA3ABC<eor>', undefined], ['INSERT', '<call:4>W9XY<eor>', undefined]],
    'one insert a record, in order, and never with REPLACE');

  // A refused key stops the batch, with what was done before it.
  calls.length = 0;
  answers = ['RESULT=OK&LOGID=5&COUNT=1', 'STATUS=AUTH&RESULT=AUTH&REASON=invalid api key\n&EXTENDED='];
  assert.deepStrictEqual(await ask('/qrz/insert', { key: KEY, records: ['<call:4>AA1A<eor>', '<call:4>AA1B<eor>', '<call:4>AA1C<eor>'] }),
    { ok: false, error: 'auth', reason: 'invalid api key', results: [{ result: 'inserted', logid: '5' }], http: 200 });
  assert.strictEqual(calls.length, 2, 'the third record is not sent');

  // QRZ not answering is said so, without the error's text.
  answers = [new Error('connect failed to logbook.qrz.com with KEY=' + KEY)];
  assert.deepStrictEqual(await ask('/qrz/status', { key: KEY }), { ok: false, error: 'unreachable', http: 502 });

  // Refused before QRZ is asked.
  calls.length = 0;
  const many = Array.from({ length: 11 }, () => '<call:4>AA1A<eor>');  // one over QRZ_MAX_INSERTS
  for (const [p, body, status, error] of [
    ['/qrz/status', '{not json', 400, 'bad_request'],
    ['/qrz/status', {}, 400, 'bad_key'],
    ['/qrz/status', { key: 'x' }, 400, 'bad_key'],
    ['/qrz/status', { key: 'has spaces and <tags>' }, 400, 'bad_key'],
    ['/qrz/delete', { key: KEY, logids: '1' }, 404, 'not_found'],
    ['/qrz/', { key: KEY }, 404, 'not_found'],
    ['/qrz/fetch', { key: KEY, from: '2026-01-01', to: 'tomorrow' }, 400, 'bad_request'],
    ['/qrz/fetch', { key: KEY, from: '2026-01-01+2026-01-02,ALL', to: '2026-01-02' }, 400, 'bad_request'],
    ['/qrz/fetch', { key: KEY, from: '2026-01-01', to: '2026-01-02', after: '1,ALL' }, 400, 'bad_request'],
    ['/qrz/insert', { key: KEY, records: [] }, 400, 'bad_request'],
    ['/qrz/insert', { key: KEY, records: many }, 400, 'bad_request'],
    ['/qrz/insert', { key: KEY, records: ['x'.repeat(2001)] }, 400, 'bad_request'],
    ['/qrz/insert', { key: KEY, records: [{ adif: '<call:4>AA1A<eor>' }] }, 400, 'bad_request'],
    ['/qrz/insert', { key: KEY, records: 'one' }, 400, 'bad_request'],
    ['/qrz/status', JSON.stringify({ key: KEY, pad: 'x'.repeat(70000) }), 413, 'too_large'],
  ]) {
    const r = await ask(p, body);
    assert.deepStrictEqual([r.http, r.ok, r.error], [status, false, error], p + ' ' + JSON.stringify(body).slice(0, 60));
  }
  assert.strictEqual((await post('/qrz/status', null, 'GET')).status, 405);
  assert.deepStrictEqual(calls, [], 'none of these reached QRZ');
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
