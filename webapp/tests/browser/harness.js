// The browser harness (docs/TEST_PLAN.md, Automated: browser): the web app as it ships, in headless Chrome,
// against host_mock/sim_station behind tools/serve_webapp_local.py, with the QRZ relay stood in for. Each
// webapp/tests/browser/test_*.js starts one station (start()), opens as many browsers on it as it needs
// (browser()), and reports through check(). tools/check.sh's browser check builds the app and the simulator
// first.
//
// Chrome is the one already installed (playwright-core, from webapp/tools, downloads none): $CHROME, else
// the usual places on macOS, Windows and Linux.
'use strict';
const path = require('path');
const fs = require('fs');
const net = require('net');
const { spawn, spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const { chromium } = require(path.join(ROOT, 'webapp', 'tools', 'node_modules', 'playwright-core'));
const TOKEN = 'sim-station-token';  // host_mock/sim_station.cpp, kToken

function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const places = [
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
    path.join(process.env.PROGRAMFILES || 'C:\\Program Files', 'Google', 'Chrome', 'Application', 'chrome.exe'),
    path.join(process.env['PROGRAMFILES(X86)'] || 'C:\\Program Files (x86)', 'Google', 'Chrome', 'Application', 'chrome.exe'),
    path.join(process.env.LOCALAPPDATA || '', 'Google', 'Chrome', 'Application', 'chrome.exe'),
  ];
  for (const p of places) if (p && fs.existsSync(p)) return p;
  const which = process.platform === 'win32' ? 'where' : 'which';
  for (const name of ['google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser']) {
    const r = spawnSync(which, [name], { encoding: 'utf8' });
    if (r.status === 0 && r.stdout.trim()) return r.stdout.trim().split(/\r?\n/)[0];
  }
  return null;
}

function freePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.once('error', reject);
    srv.listen(0, '127.0.0.1', () => { const { port } = srv.address(); srv.close(() => resolve(port)); });
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Polls fn until it returns something truthy, or ms run out; returns the last value either way.
async function until(fn, ms) {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn();
    if (v || Date.now() > end) return v;
    await sleep(300);
  }
}

let passed = 0, failed = 0;
function check(name, ok, detail) {
  if (ok) { passed++; console.log('PASS  ' + name); return; }
  failed++;
  console.log('FAIL  ' + name + (detail !== undefined ? '  -- ' + JSON.stringify(detail) : ''));
}

// One station: the preview server serving the built app (webapp/dist/app) and proxying /api/ to the
// simulator, at `scale` times real time. Returns its origin and helpers to read and write it as the
// sidekick would.
async function start(opts) {
  opts = opts || {};
  const chrome = findChrome();
  if (!chrome) {
    console.log('SKIP: no Chrome found; set CHROME to its path');
    process.exit(3);
  }
  if (!fs.existsSync(path.join(ROOT, 'webapp', 'dist', 'app', 'app.html'))) {
    throw new Error('no webapp/dist/app: run python3 webapp/tools/build.py');
  }
  const port = await freePort();
  const simPort = await freePort();
  const python = process.env.PYTHON || (process.platform === 'win32' ? 'python' : 'python3');
  const server = spawn(python, [path.join(ROOT, 'tools', 'serve_webapp_local.py'), '--dist', '--sim',
    '--scale', String(opts.scale || 15), '--port', String(port), '--sim-port', String(simPort)],
  { cwd: ROOT, stdio: ['ignore', 'ignore', 'inherit'] });
  const origin = 'http://127.0.0.1:' + port;
  const station = async () => (await fetch(origin + '/api/config')).json();
  const up = await until(async () => { try { return (await station()).ok === true; } catch (e) { return false; } }, 15000);
  if (!up) { server.kill(); throw new Error('the simulator did not come up on ' + origin); }

  const b = await chromium.launch({ executablePath: chrome, args: ['--headless=new'] });
  const errors = [];
  const pages = [];
  // SIGINT, not SIGTERM: the server's KeyboardInterrupt runs its atexit, which stops the simulator.
  async function stop() {
    await b.close().catch(() => {});
    if (process.platform === 'win32') server.kill(); else server.kill('SIGINT');
    await new Promise((r) => { server.once('exit', r); setTimeout(r, 3000); });
  }

  // A browser of its own (its own storage, so its own pairing), on app.html. paired false: it never
  // gets the token. relay(path, body) answers the QRZ relay's /qrz/* calls; every call is in .relayCalls.
  async function browser(name, bopts) {
    bopts = bopts || {};
    const ctx = await b.newContext();
    const relayCalls = [];
    await ctx.route('https://micro-ft8.kalikstein.com/**', async (route) => {
      const p = new URL(route.request().url()).pathname;
      if (!p.startsWith('/qrz/') || !bopts.relay) return route.fulfill({ status: 404, body: '' });
      const body = JSON.parse(route.request().postData() || '{}');
      relayCalls.push({ path: p, body });
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify(await bopts.relay(p, body)) });
    });
    if (bopts.paired === false) await ctx.route(origin + '/api/pairing-token', (r) => r.fulfill({ status: 404, body: '' }));
    const configReads = [];
    ctx.on('response', async (r) => {
      if (new URL(r.url()).pathname === '/api/config' && r.request().method() === 'GET') {
        try { configReads.push(await r.text()); } catch (e) { /* the page went */ }
      }
    });
    const page = await ctx.newPage();
    page.on('pageerror', (e) => errors.push(name + ': ' + e.message));
    await page.goto(origin + '/app/app.html');
    await page.waitForFunction(() => typeof lastCfg === 'object' && lastCfg.call !== undefined, null, { timeout: 15000 });
    // Toasts, as the operator would read them.
    await page.evaluate(() => {
      window.__toasts = [];
      var orig = window.failMsg;
      window.failMsg = function (t) { window.__toasts.push(String(t)); return orig(t); };
    });
    page.relayCalls = relayCalls;
    page.configReads = configReads;
    page.toasts = () => page.evaluate(() => window.__toasts.slice());
    page.settings = (pg) => page.evaluate((x) => { showSetPage(x); showView('settings'); }, pg);
    pages.push(page);
    return page;
  }

  return {
    origin, browser, stop, errors, pages, station,
    put: (body) => fetch(origin + '/api/config', { method: 'PUT', headers: { 'X-MicroFT8-Token': TOKEN }, body }),
  };
}

// Runs a test body against a fresh station, and exits 0 only if every check passed.
function run(body, opts) {
  (async () => {
    const st = await start(opts);
    try {
      await body(st);
      check('no page errors', st.errors.length === 0, st.errors);
    } finally {
      await st.stop();
    }
    console.log(passed + ' passed, ' + failed + ' failed');
    process.exit(failed ? 1 : 0);
  })().catch((e) => { console.error(e); process.exit(2); });
}

module.exports = { run, check, until, sleep, TOKEN };
