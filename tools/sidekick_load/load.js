// PAGES phone-sized pages on the sidekick's app (3), each reloaded every RELOAD_S seconds (20), staggered, for
// MINUTES. With no token the pages run unpaired, as a phone that never paired: read-only, and asking for a token
// every second. TOKEN_FILE=webapp/tools/.sidekick_token pairs them, as an operator's phone is: the app then sets
// the station's clock now and then, and writes nothing else unless asked. README.md says how it is used.
//   [TOKEN_FILE=...] node tools/sidekick_load/load.js http://192.168.50.128 10 [PAGES] [RELOAD_S]
// Needs Playwright (`npm i playwright` and `npx playwright install chromium`); CHROME=<path> uses an installed
// Chrome instead of Playwright's own.
const { chromium } = require('playwright');
const HOST = (process.argv[2] || 'http://micro-ft8.local').replace(/\/$/, '');
const MIN = Number(process.argv[3] || 10);
const PAGES = Number(process.argv[4] || 3);
const RELOAD_MS = 1000 * Number(process.argv[5] || 20);
const now = () => new Date().toISOString().slice(11, 19) + 'Z';
// The pairing token, put where the app keeps it (sidekick/main/web/pairing.js) before any page loads.
const TOKEN = process.env.TOKEN_FILE ? require('fs').readFileSync(process.env.TOKEN_FILE, 'utf8').trim() : '';
(async () => {
  const b = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : {});
  const pages = [];
  for (let i = 0; i < PAGES; i++) {
    const ctx = await b.newContext({ viewport: { width: 390, height: 844 } });
    if (TOKEN) await ctx.addInitScript((t) => localStorage.setItem('microft8_token', t), TOKEN);
    const p = await ctx.newPage();
    await p.goto(HOST + '/app.html').catch(e => console.log(now(), 'page', i, 'load failed', e.message));
    pages.push(p);
  }
  console.log(now(), PAGES, TOKEN ? 'paired' : 'unpaired', 'pages open');
  const end = Date.now() + MIN * 60000; let n = 0, fails = 0;
  while (Date.now() < end) {
    for (let i = 0; i < PAGES && Date.now() < end; i++) {
      await new Promise(r => setTimeout(r, Math.min(RELOAD_MS / PAGES, end - Date.now())));
      const t0 = Date.now();
      try { await pages[i].reload({ waitUntil: 'load', timeout: 30000 }); n++; console.log(now(), 'reload', i, Date.now() - t0, 'ms'); }
      catch (e) { fails++; console.log(now(), 'reload', i, 'failed', e.message.split('\n')[0]); }
    }
  }
  console.log(now(), 'done', n, 'reloads', fails, 'failed'); await b.close();
})();
