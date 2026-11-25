#!/usr/bin/env node
// Settings › Help & about (docs/architecture/webapp.md), in one browser on a simulated station: Versions
// shows what the station says it runs, the email carries those same lines, a note says the links need
// internet when the update check found none, Share copies the site's address and a tap on Versions copies them, Licences is read from the
// app as it ships (licences.json) and a row opens its text, and Back from Licences returns to Help & about.
// node webapp/tests/test_help.js holds the link's rules; this holds the wiring.
'use strict';
const { run, check, until } = require('./harness');

run(async (st) => {
  const A = await st.browser('A');
  await A.settings('about');
  const cfg = await st.station();

  const shown = await until(async () => {
    const v = await A.textContent('#netVersion');
    return v && v !== '—' ? v : null;
  }, 10000);
  check('Versions shows the sidekick\'s version', !!shown && shown.includes(String(cfg.sk_fw_version || '').split('+').pop().slice(0, 7)),
    [shown, cfg.sk_fw_version]);

  check('one GitHub link, to its choice of a report or a feature',
    (await A.getAttribute('#helpGithub', 'href')) === 'https://github.com/kb2slo/micro-ft8/issues/new/choose');
  const mail = await A.getAttribute('#helpEmail', 'href');
  check('Email is to the project\'s address, with the versions in the body',
    mail.startsWith('mailto:micro-ft8@kalikstein.com?') && decodeURIComponent(mail).includes('Sidekick and app: ' + shown), mail);

  // The harness answers the site with 404s, so the update check finds no internet, as on the sidekick's own network.
  check('with no internet, a note says the links need it', !!(await until(() => A.evaluate(() => !document.getElementById('helpOffline').hidden), 5000)));

  // Share, as on plain HTTP: no share sheet, so the link is copied and a toast says so. The harness serves
  // the app on 127.0.0.1, which a browser counts as secure, so the sheet is taken away to stand in for HTTP.
  await A.context().grantPermissions(['clipboard-read', 'clipboard-write']);
  await A.evaluate(() => { delete Navigator.prototype.share; });
  await A.click('#helpShare');
  const copied = await A.evaluate(() => navigator.clipboard.readText());
  check('Share copies the site\'s address', copied === 'https://micro-ft8.kalikstein.com/', copied);
  check('and a toast says so', (await A.textContent('#opToastText')) === 'Link copied: https://micro-ft8.kalikstein.com/',
    await A.textContent('#opToastText'));

  await A.click('#helpVersions');
  const versionsCopied = await A.evaluate(() => navigator.clipboard.readText());
  check('a tap on Versions copies them', versionsCopied.startsWith('Sidekick and app: ' + shown), versionsCopied);
  check('and a toast says so', (await A.textContent('#opToastText')) === 'Versions copied');

  await A.click('#setp-about button.navrow[data-page=licences]');
  const rows = await until(async () => {
    const n = await A.evaluate(() => document.querySelectorAll('#licList .licitem').length);
    return n > 5 ? n : null;
  }, 10000);
  check('Licences lists the projects from licences.json', !!rows, rows);
  check('Mini-FT8 is first', (await A.textContent('#licList .licitem summary b')) === 'Mini-FT8');
  await A.click('#licList .licitem summary');
  // The text goes in on the details' toggle event, which fires after the tap.
  const text = await until(() => A.evaluate(() => (document.querySelector('#licList .licitem pre') || {}).textContent || ''), 3000) || '';
  check('a tap opens its text', /Permission is hereby granted/.test(text), text.slice(0, 80));

  await A.click('#setp-licences .setback');
  check('Back from Licences returns to Help & about', await A.evaluate(() => !document.getElementById('setp-about').hidden
    && document.getElementById('setp-licences').hidden));
  await A.click('#setp-about .setback');
  check('and Back again to the menu', await A.evaluate(() => !document.getElementById('setRoot').hidden));
});
