// Three phone-sized pages on the sidekick's app, each reloaded every 20 s (staggered), for MINUTES.
// No token: the app runs read-only, so nothing is written to the station. README.md says how it is used.
//   node tools/sidekick_load/load.js http://192.168.50.128 10
// Needs Playwright (`npm i playwright` and `npx playwright install chromium`); CHROME=<path> uses an installed
// Chrome instead of Playwright's own.
const { chromium } = require('playwright');
const HOST = (process.argv[2] || 'http://micro-ft8.local').replace(/\/$/, '');
const MIN = Number(process.argv[3] || 10);
const now = () => new Date().toISOString().slice(11, 19) + 'Z';
(async () => {
  const b = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : {});
  const pages = [];
  for (let i = 0; i < 3; i++) {
    const p = await (await b.newContext({ viewport: { width: 390, height: 844 } })).newPage();
    await p.goto(HOST + '/app.html').catch(e => console.log(now(), 'page', i, 'load failed', e.message));
    pages.push(p);
  }
  console.log(now(), 'three pages open');
  const end = Date.now() + MIN * 60000; let n = 0, fails = 0;
  while (Date.now() < end) {
    for (let i = 0; i < 3 && Date.now() < end; i++) {
      await new Promise(r => setTimeout(r, 20000 / 3));
      const t0 = Date.now();
      try { await pages[i].reload({ waitUntil: 'load', timeout: 30000 }); n++; console.log(now(), 'reload', i, Date.now() - t0, 'ms'); }
      catch (e) { fails++; console.log(now(), 'reload', i, 'failed', e.message.split('\n')[0]); }
    }
  }
  console.log(now(), 'done', n, 'reloads', fails, 'failed'); await b.close();
})();
