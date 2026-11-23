#!/usr/bin/env node
// Spotting (docs/architecture/webapp.md, Spotting), end to end in two browsers on one simulated station, the
// site's /pskr/report route stood in for: the switch is the station's, off to start; turned on in one browser
// the station holds it and the other shows it; each page then posts the decodes it saw, under the station's
// call and grid, and the line under the switch says how it went; turned off, nothing more is posted.
'use strict';
const { run, check, until, sleep } = require('./harness');

run(async (st) => {
  const relay = (p, body) => {
    if (p !== '/pskr/report') return { ok: false, error: 'not_found' };
    return { ok: true, sent: body.spots.length, retry_in: 5 * 60 * 1000 };
  };
  const posts = (page) => page.relayCalls.filter((c) => c.path === '/pskr/report');

  const A = await st.browser('A', { relay });
  await A.settings('station');
  check('off to start', (await A.inputValue('#pskr_report')) === '0' && (await st.station()).pskr_report === '0');
  check('no line while off', await A.isHidden('#pskrReportLine'));

  await A.selectOption('#pskr_report', '1');
  check('the station holds it on', await until(async () => (await st.station()).pskr_report === '1', 5000), await st.station());
  const B = await st.browser('B', { relay });
  await B.settings('station');
  check('another browser shows it on', (await B.inputValue('#pskr_report')) === '1');

  // A first post comes within 30 s of the first spot, on a 10 s tick.
  check('A posts what it decoded', await until(async () => posts(A).length > 0, 60000), A.relayCalls);
  const cfg = await st.station();
  const body = posts(A)[0].body;
  check('under the station\'s call and grid', body.call === cfg.call && body.grid === cfg.grid, [body.call, body.grid]);
  check('as spots: a sender, Hz, SNR, mode and time', body.spots.length > 0 && body.spots.every((s) =>
    /^[A-Z0-9/]+$/.test(s.c) && Number.isInteger(s.f) && s.f > 1e6 && Number.isInteger(s.s) && /^FT[48]$/.test(s.m)
    && Number.isInteger(s.t) && s.t > 1.7e9), body.spots.slice(0, 3));
  check('one spot a sender', new Set(body.spots.map((s) => s.c)).size === body.spots.length);
  const line = await until(async () => /^Sent \d+ spots?, just now\.$/.test(await A.textContent('#pskrReportLine')), 12000);
  check('the line says what went', line, await A.textContent('#pskrReportLine'));
  check('B posts too: the route makes them one report', await until(async () => posts(B).length > 0, 60000), B.relayCalls);

  // Turned off: the line goes, and nothing more is posted.
  await B.selectOption('#pskr_report', '0');
  check('the station holds it off', await until(async () => (await st.station()).pskr_report === '0', 5000));
  check('A\'s line goes', await until(() => A.isHidden('#pskrReportLine'), 20000));
  const n = posts(A).length + posts(B).length;
  await sleep(25000);
  check('nothing more is posted', posts(A).length + posts(B).length === n);
});
