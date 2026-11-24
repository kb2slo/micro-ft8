#!/usr/bin/env node
// The landing page's screenshots and its short clip, captured from a real station
// (docs/architecture/webapp.md, The landing page's screenshots).
//
//   node webapp/tools/site_shots.mjs watch               reads only: ui-qsos.jpeg, ui-summit.jpeg
//   node webapp/tools/site_shots.mjs reply --transmit    TRANSMITS: replies to CQs. ui-operate.jpeg,
//                                                        ui-reply-1..3.jpeg, ui-heard.jpeg, reply-clip.mp4
//   node webapp/tools/site_shots.mjs sweep --transmit    TRANSMITS: one SWR sweep. ui-swr.jpeg
//
//   --station URL     the sidekick (default http://micro-ft8.local)
//   --out DIR         where the images go (default webapp/img)
//   --frames DIR      where every frame of a reply session is kept (default webapp/dist/site_shots)
//   --replies N       reply: CQs to answer, 1 to 3 (default 2: one FT8, one FT4)
//   --bands 30m,60m   sweep: bands to tick besides the ones Settings has
//   --summit-ref REF  watch: the reference the summit shot shows (default 9N/KH-001, which is made up)
//
// It opens the app the sidekick serves in headless Chrome at a phone's size, paired with the token
// webapp/tools/sidekick.py caches (or by a press of the pairing button when there is none). The browser
// may read the station freely. It may write only what its command names: the clock sync the app sends by
// itself, a stop, and for reply and sweep the replies or the one sweep start. Everything else is refused
// and listed at the end. Requests to other hosts are refused too, so a capture never posts spots or
// syncs a logbook, but for the PSKReporter feed and the site's map data during reply.
//
// reply and sweep key the radio, so they need --transmit and the station's control operator at the
// station. An agent runs them only with that person's say-so in that turn (AGENTS.md, Read the station
// before asking about it). reply-clip.mp4 needs ffmpeg on the PATH; without it the stills are still made.
//
// Needs: npm ci --prefix webapp/tools, and Chrome or Chromium (CHROME=/path when it is not found).

import fs from 'node:fs';
import path from 'node:path';
import dns from 'node:dns/promises';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright-core');
const { findChrome } = require('../tests/browser/harness.js');

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const TOKEN_CACHE = path.join(ROOT, 'webapp', 'tools', '.sidekick_token');
const SITE = 'micro-ft8.kalikstein.com';
const PHONE = { width: 402, height: 874 };   // the iPhone the app's layout was measured on (webapp.md)
const JPEG = { type: 'jpeg', quality: 85 };

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function parseArgs(argv) {
  const a = { what: argv[0], station: 'http://micro-ft8.local', out: path.join(ROOT, 'webapp', 'img'),
    frames: path.join(ROOT, 'webapp', 'dist', 'site_shots'), transmit: false, replies: 2, bands: [],
    summitRef: '9N/KH-001' };
  for (let i = 1; i < argv.length; i++) {
    const k = argv[i], v = () => { if (i + 1 >= argv.length) throw new Error(k + ' needs a value'); return argv[++i]; };
    if (k === '--station') a.station = v();
    else if (k === '--out') a.out = path.resolve(v());
    else if (k === '--frames') a.frames = path.resolve(v());
    else if (k === '--transmit') a.transmit = true;
    else if (k === '--replies') a.replies = Math.max(1, Math.min(3, Number(v()) || 2));
    else if (k === '--bands') a.bands = v().split(',').map((s) => s.trim()).filter(Boolean);
    else if (k === '--summit-ref') a.summitRef = v();
    else throw new Error('unknown option ' + k);
  }
  return a;
}

// Chrome under a sandbox may not resolve .local names, so the name is looked up here and the browser is
// given the address.
async function resolveStation(url) {
  const u = new URL(url);
  const { address } = await dns.lookup(u.hostname, { family: 4 });
  return { origin: u.protocol + '//' + address + (u.port ? ':' + u.port : ''), host: address };
}

// One browser on the station. allow(method, pathname) lets a write through; allowHost(url) lets a request
// to another host through; holdSite leaves the site's requests unanswered, so a page shows no error for
// a service this browser was never going to call.
async function openStation(args, opts) {
  opts = opts || {};
  const chrome = findChrome();
  if (!chrome) throw new Error('No Chrome or Chromium found. Install one, or set CHROME=/path/to/chrome.');
  const st = await resolveStation(args.station);
  const token = fs.existsSync(TOKEN_CACHE) ? fs.readFileSync(TOKEN_CACHE, 'utf8').trim() : '';
  const browser = await chromium.launch({ executablePath: chrome, args: ['--headless=new'] });
  const viewport = opts.landscape ? { width: PHONE.height, height: PHONE.width } : PHONE;
  const ctx = await browser.newContext({ viewport, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
  if (token) await ctx.addInitScript((t) => { try { localStorage.setItem('microft8_token', t); } catch (e) { /* no storage */ } }, token);
  const log = { sent: [], refused: [], hosts: new Set() };
  await ctx.route('**/*', (route) => {
    const req = route.request(), u = new URL(req.url());
    if (u.hostname !== st.host) {
      if (opts.allowHost && opts.allowHost(u)) return route.continue();
      if (opts.holdSite && u.hostname === SITE) return undefined;   // held: never answered
      log.hosts.add(u.hostname);
      return route.abort();
    }
    if (req.method() === 'GET') return route.continue();
    const what = req.method() + ' ' + u.pathname + ' ' + (req.postData() || '');
    const ok = u.pathname === '/api/time' || u.pathname === '/api/tx/cancel' || u.pathname === '/api/queue/cancel' ||
      (opts.allow && opts.allow(req.method(), u.pathname));
    if (!ok) { log.refused.push(what); return route.abort(); }
    log.sent.push(new Date().toISOString() + ' ' + what);
    if (u.pathname !== '/api/time') console.log('WRITE  ' + what);
    return route.continue();
  });
  const report = () => {
    console.log('Sent to the station: ' + (log.sent.length ? '\n  ' + log.sent.join('\n  ') : 'nothing'));
    if (log.refused.length) console.log('Refused: ' + [...new Set(log.refused)].join('; '));
    if (log.hosts.size) console.log('Other hosts refused: ' + [...log.hosts].join(', '));
  };
  return { browser, ctx, origin: st.origin, paired: !!token, report };
}

// The app, loaded and paired. With no cached token the page pairs itself once the sidekick's button is
// pressed, as a phone does.
async function openApp(station, page) {
  page = page || 'app.html';
  const p = await station.ctx.newPage();
  await p.goto(station.origin + '/' + page, { waitUntil: 'domcontentloaded' });
  if (!station.paired) {
    console.log('No cached pairing token: press the pairing button on the sidekick now (waiting 90 s).');
    await p.waitForFunction(() => !!localStorage.getItem('microft8_token'), null, { timeout: 90000 });
  }
  return p;
}

// Requests still on their way when the browser goes are let go, not reported as errors.
async function closeStation(st) {
  st.report();
  await st.ctx.unrouteAll({ behavior: 'ignoreErrors' }).catch(() => {});
  await st.browser.close();
}

const shown = (file) => (file.startsWith(ROOT + path.sep) ? path.relative(ROOT, file) : file);

function save(args, name, from) {
  fs.mkdirSync(args.out, { recursive: true });
  const to = path.join(args.out, name);
  fs.copyFileSync(from, to);
  console.log('wrote ' + shown(to) + '  <-  ' + path.basename(from));
}

// ---- watch: reads only ----

async function watch(args) {
  fs.mkdirSync(args.frames, { recursive: true });
  // The QSOs list, scrolled to the first place two neighbouring contacts differ in mode.
  let st = await openStation(args, { holdSite: true });
  let p = await openApp(st);
  await sleep(6000);
  await p.tap('#tabbar button[data-view="log"]');
  await sleep(5000);
  const met = await p.evaluate(() => {
    const rows = [...document.querySelectorAll('#view-log .frow')];
    const mode = (r) => (/\bFT4\b/.test(r.textContent) ? 'FT4' : /\bFT8\b/.test(r.textContent) ? 'FT8' : '');
    for (let i = 1; i < rows.length; i++) {
      if (mode(rows[i]) && mode(rows[i - 1]) && mode(rows[i]) !== mode(rows[i - 1])) { rows[i].scrollIntoView({ block: 'center' }); return true; }
    }
    return false;
  });
  if (!met) console.log('The days loaded hold one mode only, so ui-qsos.jpeg shows the top of the list.');
  await sleep(1200);
  let f = path.join(args.frames, 'qsos.jpg');
  await p.screenshot({ path: f, ...JPEG });
  save(args, 'ui-qsos.jpeg', f);
  await closeStation(st);

  // Operate as it reads on a summit. The reference and the three contacts logged at it exist in this
  // browser only: the station's config and today's log are read as they are and changed on the way to
  // the page, and nothing is written to the station.
  st = await openStation(args, { holdSite: true });
  await st.ctx.route('**/api/config', async (route) => {
    if (route.request().method() !== 'GET') return route.fallback();
    const r = await route.fetch(), j = await r.json();
    j.act_refs = 'SOTA:' + args.summitRef;
    return route.fulfill({ response: r, json: j });
  });
  await st.ctx.route('**/api/log/entries**', async (route) => {
    const r = await route.fetch(), j = await r.json();
    const rows = j.entries || j.rows || [];
    if (!Number(new URL(route.request().url()).searchParams.get('skip') || 0)) rows.slice(-3).forEach((q) => { q.my_sota_ref = args.summitRef; });
    return route.fulfill({ response: r, json: j });
  });
  p = await openApp(st);
  await sleep(20000);   // a slot of decodes, and the day's log read
  const line = await p.evaluate(() => (document.querySelector('#opActLine') || {}).textContent || '');
  console.log('summit line reads: ' + (line.trim() || '(nothing: today has fewer than one logged contact?)'));
  f = path.join(args.frames, 'summit.jpg');
  await p.screenshot({ path: f, ...JPEG });
  save(args, 'ui-summit.jpeg', f);
  await closeStation(st);
}

// ---- reply: transmits ----

async function reply(args) {
  if (!args.transmit) throw new Error('reply answers CQs on the air: pass --transmit, with the control operator at the station.');
  fs.rmSync(args.frames, { recursive: true, force: true });
  fs.mkdirSync(args.frames, { recursive: true });
  let replies = 0;
  const st = await openStation(args, {
    allow: (m, p) => { if (p === '/api/queue/reply' && replies < args.replies) { replies++; return true; } return false; },
    allowHost: (u) => /(^|\.)pskreporter\.info$/.test(u.hostname) || (u.hostname === SITE && u.pathname.startsWith('/data/')),
  });
  const p = await openApp(st);
  const map = await st.ctx.newPage();   // listens for who hears us from the first transmission on
  await map.goto(st.origin + '/map.html?layer=psk', { waitUntil: 'domcontentloaded' });
  await p.bringToFront();

  const t0 = Date.now(), frames = [];
  const note = (s) => console.log(((Date.now() - t0) / 1000).toFixed(1).padStart(6) + 's  ' + s);
  const state = () => p.evaluate(() => ({
    queue: [...document.querySelectorAll('#opQueueRows > *')].map((r) => r.textContent.replace(/\s+/g, ' ').trim()),
    me: document.querySelectorAll('#opDecodeRows .me').length,
    sending: /Sending/.test((document.querySelector('#opRadioBar') || {}).textContent || ''),
  }));
  let shooting = true;
  const shooter = (async () => {
    while (shooting) {
      const file = path.join(args.frames, 'f_' + String(frames.length).padStart(5, '0') + '.jpg');
      try { await p.screenshot({ path: file, ...JPEG }); frames.push({ file, t: Date.now() - t0, ...(await state()) }); } catch (e) { /* the page was busy */ }
      await sleep(300);
    }
  })();

  // The strongest plain CQ (no modifier, not worked, not already called) in the newest slot of a mode.
  const pick = (mode, skip) => p.evaluate(([mode, skip]) => {
    let best = null;
    for (const s of document.querySelectorAll('#opDecodeRows .dslot')) {
      const head = s.firstElementChild ? s.firstElementChild.textContent : '';
      if (mode && !head.includes(mode)) continue;
      for (const r of s.querySelectorAll('.drow.tap')) {
        const txt = ((r.querySelector('.txt') || {}).textContent || '').trim();
        const m = /^CQ ([A-Z0-9/]{3,}) [A-R]{2}\d{2}$/.exec(txt);
        if (!m || skip.includes(m[1]) || /Worked|Queued|Sending/.test(r.textContent)) continue;
        const snr = parseInt((r.querySelector('.dsnr') || {}).textContent || '-99', 10);
        if (snr >= -10 && (!best || snr > best.snr)) best = { el: r, snr, call: m[1], txt };
      }
      if (best) break;
    }
    if (!best) return null;
    best.el.scrollIntoView({ block: 'center' });
    const q = best.el.getBoundingClientRect();
    return { x: q.right - 70, y: q.top + q.height / 2, snr: best.snr, call: best.call, txt: best.txt };   // clear of the call and grid links
  }, [mode, skip]);
  const tap = async (t) => {
    await p.evaluate(({ x, y }) => {   // a dot where the finger lands, for the clip
      const d = document.createElement('div');
      d.style.cssText = 'position:fixed;z-index:99999;pointer-events:none;width:46px;height:46px;border-radius:50%;' +
        'background:rgba(29,78,216,.28);border:2px solid rgba(29,78,216,.7);left:' + (x - 23) + 'px;top:' + (y - 23) + 'px;transition:opacity .5s .5s';
      document.body.appendChild(d); requestAnimationFrame(() => { d.style.opacity = '0'; }); setTimeout(() => d.remove(), 1200);
    }, t);
    await sleep(250);
    await p.touchscreen.tap(t.x, t.y);
  };
  const called = [], taps = [];
  const tryReply = async (mode, waitMs) => {
    const end = Date.now() + waitMs;
    while (Date.now() < end && replies < args.replies) {
      const first = await pick(mode, called);
      if (first) {
        await sleep(600);   // the list may have redrawn under the row: look again before tapping
        const t = await pick(mode, called);
        if (!t || t.call !== first.call) continue;
        note('tap ' + (mode || 'any mode') + ': "' + t.txt + '" at ' + t.snr + ' dB');
        called.push(t.call); taps.push(Date.now() - t0);
        await tap(t); await sleep(3500);
        return true;
      }
      await sleep(1500);
    }
    note('no plain CQ to answer in ' + (mode || 'any mode'));
    return false;
  };

  await sleep(9000);
  await tryReply('FT8', 75000);
  if (args.replies > 1) await tryReply('FT4', 60000);
  // Until the queue has been empty for 20 s, or 7.5 minutes. One more call if nobody answered.
  let sawMe = false, emptySince = 0;
  while (Date.now() - t0 < 7.5 * 60 * 1000) {
    const s = await state();
    if (s.me) sawMe = true;
    emptySince = s.queue.length ? 0 : (emptySince || Date.now());
    if (emptySince && Date.now() - emptySince > 20000) {
      if (!sawMe && replies < args.replies && Date.now() - t0 < 4.5 * 60 * 1000 && (await tryReply('', 45000))) { emptySince = 0; continue; }
      break;
    }
    await sleep(2000);
  }
  note('queue finished; ' + replies + ' replies sent');
  await sleep(3000);
  shooting = false; await shooter;
  await map.bringToFront(); await sleep(2500);
  const mapFile = path.join(args.frames, 'heard.jpg');
  await map.screenshot({ path: mapFile, ...JPEG });
  await closeStation(st);
  fs.writeFileSync(path.join(args.frames, 'frames.json'), JSON.stringify({ taps, frames }, null, 1));
  if (!taps.length) throw new Error('Nothing was answered, so there is nothing to pick from. The frames are in ' + args.frames);

  // The picks, by rule. Every frame is kept, so a better moment can be copied over a pick by hand.
  const at = (ms) => frames.find((f) => f.t >= ms) || frames[frames.length - 1];
  const before = [...frames].reverse().find((f) => f.t < taps[0]) || frames[0];
  const queued = at(taps[0] + 2500);
  const lastTap = taps[taps.length - 1];
  const done = frames.find((f) => f.t > lastTap && f.queue.some((q) => /SIGNOFF/.test(q))) || [...frames].reverse().find((f) => f.me) || frames[frames.length - 1];
  const hero = frames.find((f) => f.t > lastTap && f.me && f.queue.length >= 2 && !f.sending) ||
    frames.find((f) => f.t > lastTap && f.me && !f.sending) || frames.find((f) => f.me) || queued;
  save(args, 'ui-reply-1.jpeg', before.file);
  save(args, 'ui-reply-2.jpeg', queued.file);
  save(args, 'ui-reply-3.jpeg', done.file);
  save(args, 'ui-operate.jpeg', hero.file);
  save(args, 'ui-heard.jpeg', mapFile);

  // The clip: from just before the first tap to a few seconds after the contact is done, sped up.
  const from = Math.max(0, frames.indexOf(before) - 3), to = Math.min(frames.length - 1, frames.indexOf(done) + 8);
  if (spawnSync('ffmpeg', ['-version']).status !== 0) { console.log('No ffmpeg on the PATH, so reply-clip.mp4 was not made.'); return; }
  const clip = path.join(args.out, 'reply-clip.mp4');
  const r = spawnSync('ffmpeg', ['-loglevel', 'error', '-y', '-framerate', '10', '-start_number', String(from),
    '-i', path.join(args.frames, 'f_%05d.jpg'), '-frames:v', String(to - from + 1),
    '-vf', 'scale=540:-2:flags=lanczos,format=yuv420p', '-c:v', 'libx264', '-crf', '26', '-preset', 'slow',
    '-movflags', '+faststart', '-an', clip], { stdio: 'inherit' });
  if (r.status !== 0) throw new Error('ffmpeg could not make the clip');
  const speed = (frames[to].t - frames[from].t) / ((to - from + 1) * 100);
  console.log('wrote ' + shown(clip) + '  (' + (to - from + 1) + ' frames, about ' + speed.toFixed(1) + ' times real time)');
}

// ---- sweep: transmits ----

async function sweep(args) {
  if (!args.transmit) throw new Error('sweep keys a carrier at every point: pass --transmit, with the control operator at the station.');
  fs.mkdirSync(args.frames, { recursive: true });
  let starts = 0;
  const st = await openStation(args, { landscape: true, holdSite: true,
    allow: (m, p) => { if (p === '/api/swr/start' && starts < 1) { starts++; return true; } return false; } });
  const p = await openApp(st, 'swr.html');
  await sleep(6000);
  for (const band of args.bands) {
    const label = p.locator('#bands label', { hasText: new RegExp('^\\s*' + band + '\\s*$') });
    if (!(await label.count())) throw new Error('the sweep page offers no ' + band);
    if (!(await label.locator('input').isChecked())) await label.tap();
  }
  await sleep(800);
  const info = () => p.evaluate(() => ({
    bands: [...document.querySelectorAll('#bands input:checked')].map((i) => i.parentElement.textContent.trim()),
    estimate: document.querySelector('#estimate').textContent.trim(), status: document.querySelector('#status').textContent.trim(),
  }));
  const plan = await info();
  console.log('sweeping ' + plan.bands.join(', ') + ': ' + plan.estimate);
  const t0 = Date.now();
  await p.tap('#run');
  await sleep(4000);
  for (;;) {
    const i = await info();
    if (/^Done in|Stopped/.test(i.status)) { console.log(i.status); break; }
    if (Date.now() - t0 > 6 * 60 * 1000) throw new Error('the sweep did not finish: ' + i.status);
    await sleep(1500);
  }
  await p.evaluate(() => document.querySelector('#plotWrap').scrollIntoView({ block: 'start' }));
  await sleep(500);
  const f = path.join(args.frames, 'swr.jpg');
  await p.screenshot({ path: f, ...JPEG });
  save(args, 'ui-swr.jpeg', f);
  console.log(await p.evaluate(() => document.querySelector('#summary').innerText.replace(/[ \t]+/g, ' ')));
  await closeStation(st);
}

const commands = { watch, reply, sweep };

(async () => {
  const args = parseArgs(process.argv.slice(2));
  if (!commands[args.what]) {
    console.error('usage: node webapp/tools/site_shots.mjs watch | reply --transmit | sweep --transmit   (options: the top of this file)');
    process.exit(2);
  }
  await commands[args.what](args);
})().catch((e) => { console.error('site_shots: ' + e.message); process.exit(1); });
