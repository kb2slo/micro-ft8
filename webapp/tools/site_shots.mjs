#!/usr/bin/env node
// The landing page's screenshots and its short clip, captured from a real station
// (docs/architecture/webapp.md, The landing page's screenshots).
//
//   node webapp/tools/site_shots.mjs watch               reads only: ui-qsos.jpeg, ui-summit.jpeg
//   node webapp/tools/site_shots.mjs reply --transmit    TRANSMITS: replies to CQs. ui-operate.jpeg,
//                                                        ui-reply-1..3.jpeg, ui-heard.jpeg, reply-clip.mp4
//   node webapp/tools/site_shots.mjs tour                reads only: a walk through the QSOs list, a
//                                                        station's card, the map and the decode filters,
//                                                        added to reply-clip.mp4 after the kept contact
//        ... tour --band 40m                             CHANGES THE STATION'S BAND, and shows its decodes
//        ... tour --sweep --transmit                     TRANSMITS: ends on an SWR sweep as it runs
//   node webapp/tools/site_shots.mjs clip                no station: reply-clip.mp4 again from the kept frames
//   node webapp/tools/site_shots.mjs sweep --transmit    TRANSMITS: one SWR sweep. ui-swr.jpeg
//
//   --station URL     the sidekick (default http://micro-ft8.local)
//   --out DIR         where the images go (default webapp/img)
//   --frames DIR      where every frame of a reply session and a tour is kept (default webapp/dist/site_shots)
//   --replies N       reply: CQs to answer, 1 to 3 (default 2: one FT8, one FT4)
//   --bands 30m,60m   sweep: bands to tick besides the ones Settings has
//   --band 40m        tour: the band to change to (the station stays on it)
//   --summit-ref REF  watch: the reference the summit shot shows (default 9N/KH-001, which is made up)
//
// It opens the app the sidekick serves in headless Chrome at a phone's size, paired with the token
// webapp/tools/sidekick.py caches (or by a press of the pairing button when there is none). The browser
// may read the station freely. It may write only what its command names: the clock sync the app sends by
// itself, a stop, and for reply and sweep the replies or the one sweep start. Everything else is refused
// and listed at the end. Requests to other hosts are refused too, so a capture never posts spots or
// syncs a logbook, but for the PSKReporter feed and the site's map data during reply and tour, and the
// POTA and SOTA spot reads the map and a station's card make during tour. A sweep is refused while
// Settings › Amplifier is on, since it keys a carrier on every band it is given.
//
// reply and sweep key the radio, so they need --transmit and the station's control operator at the
// station. An agent runs them only with that person's say-so in that turn (AGENTS.md, Read the station
// before asking about it). reply-clip.mp4 needs ffmpeg on the PATH; without it the stills are still made.
// tour remakes the clip from the contact reply kept and its own walk, so the walk can be redone alone.
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
    band: '', sweep: false, summitRef: '9N/KH-001' };
  for (let i = 1; i < argv.length; i++) {
    const k = argv[i], v = () => { if (i + 1 >= argv.length) throw new Error(k + ' needs a value'); return argv[++i]; };
    if (k === '--station') a.station = v();
    else if (k === '--out') a.out = path.resolve(v());
    else if (k === '--frames') a.frames = path.resolve(v());
    else if (k === '--transmit') a.transmit = true;
    else if (k === '--replies') a.replies = Math.max(1, Math.min(3, Number(v()) || 2));
    else if (k === '--bands') a.bands = v().split(',').map((s) => s.trim()).filter(Boolean);
    else if (k === '--band') a.band = v();
    else if (k === '--sweep') a.sweep = true;
    else if (k === '--summit-ref') a.summitRef = v();
    else throw new Error('unknown option ' + k);
  }
  return a;
}

// Chrome under a sandbox may not resolve .local names, so the name is looked up here and the browser is
// given the address.
async function resolveStation(url) {
  const u = new URL(url);
  let address = '';
  for (let tries = 0; !address; tries++) {   // a .local name can miss an answer now and then
    try { ({ address } = await dns.lookup(u.hostname, { family: 4 })); } catch (e) { if (tries >= 4) throw e; await sleep(1500); }
  }
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
      (opts.allow && opts.allow(req.method(), u.pathname, req.postData() || ''));
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

// A dot where the finger lands, for the clip.
async function dot(page, x, y) {
  await page.evaluate(({ x, y }) => {
    const d = document.createElement('div');
    d.style.cssText = 'position:fixed;z-index:99999;pointer-events:none;width:46px;height:46px;border-radius:50%;' +
      'background:rgba(29,78,216,.28);border:2px solid rgba(29,78,216,.7);left:' + (x - 23) + 'px;top:' + (y - 23) + 'px;transition:opacity .5s .5s';
    document.body.appendChild(d); requestAnimationFrame(() => { d.style.opacity = '0'; }); setTimeout(() => d.remove(), 1200);
  }, { x, y });
}

const shown = (file) => (file.startsWith(ROOT + path.sep) ? path.relative(ROOT, file) : file);

function save(args, name, from) {
  fs.mkdirSync(args.out, { recursive: true });
  const to = path.join(args.out, name);
  fs.copyFileSync(from, to);
  console.log('wrote ' + shown(to) + '  <-  ' + path.basename(from));
}

// A sweep keys a carrier on every band it is given, whatever an amplifier in line is set for.
async function refuseSweepThroughAmp(st) {
  const cfg = await (await fetch(st.origin + '/api/config')).json();
  if (String(cfg.amp) === '1') throw new Error('Settings › Amplifier is on. Take the amplifier out of line and turn that switch off before a sweep.');
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
      const head = s.textContent.trim().slice(0, 40);   // "20m FT8 · 14.074 MHz…"
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
    await dot(p, t.x, t.y);
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

  // The clip's contact: from just before the first tap to a few seconds after the contact is done.
  const from = Math.max(0, frames.indexOf(before) - 3), to = Math.min(frames.length - 1, frames.indexOf(done) + 8);
  fs.writeFileSync(path.join(args.frames, 'frames.json'), JSON.stringify({ clip: { from, to }, taps, frames }, null, 1));
  buildClip(args);
}

// ---- tour: reads only ----

// A frame as fast as Chrome hands one over, about ten a second, each with its time, so the clip plays the
// walk as it happened. Screenshots, not the browser's screencast: that one is a phone's points wide, half
// the pixels of the stills.
function startFrames(current, dir, rate) {   // current(): the page on screen now; rate(): how many times real time this stretch plays
  const shots = [], missed = { n: 0, why: '' };
  let on = true;
  const done = (async () => {
    while (on) {
      const name = 't_' + String(shots.length).padStart(5, '0') + '.jpg', t = Date.now() / 1000;
      try { await current().screenshot({ path: path.join(dir, name), ...JPEG }); shots.push({ file: name, t, rate: rate() }); }
      catch (e) { missed.n++; missed.why = String(e.message).split('\n')[0]; await sleep(50); }   // between two pages, as a rule
    }
  })();
  return { shots, missed, stop: async () => { on = false; await done; } };
}

async function tour(args) {
  if (args.sweep && !args.transmit) throw new Error('tour --sweep keys a carrier at every point: pass --transmit, with the control operator at the station.');
  fs.mkdirSync(args.frames, { recursive: true });
  for (const f of fs.readdirSync(args.frames)) if (/^t_\d+\.jpg$/.test(f)) fs.rmSync(path.join(args.frames, f));
  let bandWrites = 0, sweeps = 0;
  const st = await openStation(args, { holdSite: true,
    // The drawer asks the radio for its state as it opens. A band change is three lines of config and
    // nothing else; a sweep start is one, and only when asked for.
    allow: (m, p, body) => {
      if (m === 'POST' && p === '/api/radio/read') return true;
      if (args.band && m === 'PUT' && p === '/api/config' && bandWrites < 1 &&
          body.split('\n').every((l) => /^(ft4band\d+|band\d+|band_sel)=[\d.]+$/.test(l))) { bandWrites++; return true; }
      if (args.sweep && m === 'POST' && p === '/api/swr/start' && sweeps < 1) { sweeps++; return true; }
      return false;
    },
    allowHost: (u) => /(^|\.)pskreporter\.info$/.test(u.hostname) || (u.hostname === SITE && u.pathname.startsWith('/data/')) ||
      u.hostname === 'api.pota.app' || u.hostname === 'api-db2.sota.org.uk' });
  if (args.sweep) await refuseSweepThroughAmp(st);
  const p = await openApp(st);
  await sleep(17000);   // a slot or two of decodes to walk through
  const WALK = 1.25, WAIT = 5;   // times real time: the walk itself, and its waits on the station
  let onScreen = p, rate = WALK;
  const cast = startFrames(() => onScreen, args.frames, () => rate);
  const tapOn = async (sel) => {
    const b = await (typeof sel === 'string' ? p.locator(sel).first() : sel).boundingBox();
    if (!b) throw new Error('nothing to tap at ' + sel);
    const x = b.x + b.width / 2, y = b.y + b.height / 2;
    await dot(p, x, y); await sleep(350);
    await p.touchscreen.tap(x, y);
  };
  const scroll = async (dy) => { for (let i = 0; i < 12; i++) { await p.mouse.wheel(0, dy / 12); await sleep(40); } };
  // Fingers on the map: its canvas pans on one touch and zooms on two, and a mouse wheel would not show that.
  const cdp = await st.ctx.newCDPSession(p);
  const touch = (type, pts) => cdp.send('Input.dispatchTouchEvent', { type, touchPoints: pts.map(([x, y], id) => ({ x, y, id })) });
  const pinch = async (cx, cy, from, to) => {
    const at = (d) => [[cx - d * 0.35, cy + d * 0.35], [cx + d * 0.35, cy - d * 0.35]];
    for (const [x, y] of at(from)) await dot(p, x, y);
    await sleep(300);
    await touch('touchStart', at(from));
    for (let i = 1; i <= 14; i++) { await touch('touchMove', at(from + (to - from) * i / 14)); await sleep(35); }
    await touch('touchEnd', []);
  };
  const drag = async (x0, y0, x1, y1) => {
    await dot(p, x0, y0); await sleep(300);
    await touch('touchStart', [[x0, y0]]);
    for (let i = 1; i <= 16; i++) { await touch('touchMove', [[x0 + (x1 - x0) * i / 16, y0 + (y1 - y0) * i / 16]]); await sleep(35); }
    await touch('touchEnd', []);
  };

  await p.mouse.move(PHONE.width / 2, PHONE.height / 2);
  await sleep(1500);
  // The log, and a station's card from it. The tap on its QRZ.com link is shown and the tab it opens is
  // shut unseen and unloaded: that page carries the licensee's name and address, and QRZ's advertisers.
  st.ctx.on('page', (tab) => { if (tab !== p) tab.close().catch(() => {}); });
  await tapOn('#tabbar button[data-view="log"]'); await sleep(2500);
  // The newest day's contacts take a moment to read. Before the UTC day's first contact that day is
  // yesterday's, shut under its heading with its rows in the page but covered: it is opened by its heading.
  await p.waitForFunction(() => document.querySelectorAll('#view-log .frow').length > 0, null, { timeout: 12000 }).catch(() => {});
  const covered = () => p.evaluate(() => {
    const e = document.querySelector('#view-log .frow .callink');
    if (!e) return true;
    const r = e.getBoundingClientRect(), top = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
    return !(top && e.closest('.frow').contains(top));
  });
  if (await covered()) { await tapOn('#logList .day .daytoggle'); await sleep(2500); }
  await scroll(700); await sleep(1400); await scroll(-700); await sleep(900);
  // Clear of the bars pinned above and below, wherever the scroll came to rest: a tap on the slot bar goes to Operate.
  await p.locator('#view-log .frow .callink').first().evaluate((e) => e.scrollIntoView({ block: 'center' })); await sleep(600);
  await tapOn('#view-log .frow .callink'); await sleep(3600);
  if (!(await p.locator('#callInfoCard').isVisible())) throw new Error('the callsign card did not open from the QSOs list');
  await tapOn('#callInfoQrz'); await sleep(900);
  await p.bringToFront();
  await tapOn('#callInfoClose'); await sleep(900);
  // The map with who heard us on it: two fingers in, a drag, then the QSOs alone and a tap on one.
  await tapOn('#tabbar button[data-view="tools"]'); await sleep(1600);
  await tapOn('#pskrMapLink'); await p.waitForLoadState('domcontentloaded'); await sleep(4200);
  const box = await p.locator('#mapCanvas').boundingBox();
  const mx = box.x + box.width * 0.6, my = box.y + box.height * 0.42;
  await pinch(mx, my, 70, 150); await sleep(900);
  await pinch(mx, my, 70, 150); await sleep(1200);   // 4.6 times in all: a tap on a pin needs 4 (LOG_MAP_TAP_ZOOM_MIN)
  await drag(mx + 40, my + 20, mx - 80, my + 70); await sleep(1300);
  await tapOn('#layerPsk'); await sleep(2000);
  const pin = await p.evaluate(() => {
    const r = document.getElementById('mapCanvas').getBoundingClientRect();
    // A pin low on the map, where the card will cover it: in Chrome the click that follows a touch lands on
    // whatever is under the finger by then, and above the card that is the backdrop, which closes it.
    const seen = logMapPins.filter((q) => q.source === 'QSO' && q.x > 40 && q.x < r.width - 40 && q.y > r.height * 0.66 && q.y < r.height - 150);
    seen.sort((a, b) => Math.hypot(a.x - r.width / 2, a.y - r.height * 0.74) - Math.hypot(b.x - r.width / 2, b.y - r.height * 0.74));
    return seen.length ? { x: r.left + seen[0].x, y: r.top + seen[0].y, call: seen[0].call } : null;
  });
  if (pin) {
    await dot(p, pin.x, pin.y); await sleep(350);
    await p.touchscreen.tap(pin.x, pin.y); await sleep(3800);   // a QSO's pin opens the station's card
    const opened = await p.locator('#callInfoCard').isVisible();
    console.log('tapped the QSO with ' + pin.call + (opened ? '' : ', but no card opened'));
    if (opened) { await tapOn('#callInfoClose'); await sleep(700); }
  } else console.log('no QSO pin was in view to tap');
  await p.goBack(); await p.waitForLoadState('domcontentloaded'); await sleep(4500);
  // Back on Operate. A band change, and that band's first decodes arriving, played faster.
  if (args.band) {
    await tapOn('#rbToggle'); await sleep(1800);
    const btn = p.locator('#opBandGrid .bandbtn', { hasText: new RegExp('^' + args.band + '$') });
    if (!(await btn.count())) throw new Error('the drawer offers no ' + args.band + ': it shows the bands Settings has active');
    await tapOn(btn); await sleep(2500);
    if ((await p.locator('#rbToggle').getAttribute('aria-expanded')) === 'true') { await tapOn('#rbToggle'); await sleep(900); }
    rate = WAIT;
    const heard = () => p.evaluate((band) => [...document.querySelectorAll('#opDecodeRows .dslot')].filter((s) => s.textContent.trim().startsWith(band)).length, args.band);
    for (const t0 = Date.now(); (await heard()) < 2 && Date.now() - t0 < 60000;) await sleep(1000);
    console.log('on ' + args.band + ': ' + (await heard()) + ' slots of decodes');
    await sleep(2000);
    rate = WALK;
    await sleep(1500);
  }
  // The filters, then a sort.
  await tapOn('#dvFilter'); await sleep(1300);
  await tapOn('#dvFilterSheet [data-key="reply"]'); await sleep(800);
  await tapOn('#dvFilterSheet [data-act="apply"]'); await sleep(3200);
  await tapOn('#dvSort'); await sleep(1300);
  await tapOn('#dvSortSheet [data-key="far"]'); await sleep(3500);
  // An SWR sweep as it runs, played faster, ending on its plot.
  if (args.sweep) {
    await tapOn('#tabbar button[data-view="tools"]'); await sleep(1600);
    await p.locator('#toolsSwrLink').scrollIntoViewIfNeeded();
    await tapOn('#toolsSwrLink'); await p.waitForLoadState('domcontentloaded'); await sleep(5000);
    await tapOn('#run'); await sleep(2500);
    await p.evaluate(() => document.querySelector('#runRow').scrollIntoView({ block: 'start' }));
    rate = WAIT;
    for (const t0 = Date.now(); Date.now() - t0 < 6 * 60 * 1000;) {
      const status = await p.evaluate(() => document.querySelector('#status').textContent.trim());
      if (/^Done in|Stopped/.test(status)) { console.log('sweep: ' + status); break; }
      await sleep(1000);
    }
    rate = WALK;
    await sleep(1500);
    await p.evaluate(() => document.querySelector('#plotWrap').scrollIntoView({ block: 'start', behavior: 'smooth' }));
    await sleep(4000);
  }
  await cast.stop();
  fs.writeFileSync(path.join(args.frames, 'tour.json'), JSON.stringify(cast.shots, null, 1));
  if (cast.missed.n > 20) console.log('frames missed: ' + cast.missed.n + ' (' + cast.missed.why + ')');
  console.log('tour: ' + cast.shots.length + ' frames over ' + (cast.shots[cast.shots.length - 1].t - cast.shots[0].t).toFixed(1) + ' s');
  await closeStation(st);
  buildClip(args);
}

// ---- the clip: the kept contact at ten frames a second (about four times real time), then the tour as
// it was walked, a little faster than real time and its waits much faster, a still stretch cut to 1.5 s ----

function buildClip(args) {
  const kept = path.join(args.frames, 'frames.json');
  if (!fs.existsSync(kept)) { console.log('No reply session is kept in ' + shown(args.frames) + ', so reply-clip.mp4 was not made: run reply first.'); return; }
  if (spawnSync('ffmpeg', ['-version']).status !== 0) { console.log('No ffmpeg on the PATH, so reply-clip.mp4 was not made.'); return; }
  const { clip } = JSON.parse(fs.readFileSync(kept, 'utf8'));
  const lines = [];
  let last = '', seconds = 0;
  const add = (file, dur) => { lines.push("file '" + file + "'", 'duration ' + dur.toFixed(3)); last = file; seconds += dur; };
  for (let i = clip.from; i <= clip.to; i++) add('f_' + String(i).padStart(5, '0') + '.jpg', 0.1);
  const walked = path.join(args.frames, 'tour.json');
  if (fs.existsSync(walked)) {
    const shots = JSON.parse(fs.readFileSync(walked, 'utf8'));
    shots.forEach((s, i) => add(s.file, Math.min(1.5, Math.max(0.02, i + 1 < shots.length ? (shots[i + 1].t - s.t) / (s.rate || 1) : 1.5))));
  }
  lines.push("file '" + last + "'");   // the concat reader drops the last duration without it
  fs.writeFileSync(path.join(args.frames, 'clip.txt'), lines.join('\n') + '\n');
  fs.mkdirSync(args.out, { recursive: true });
  const out = path.join(args.out, 'reply-clip.mp4');
  const r = spawnSync('ffmpeg', ['-loglevel', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', 'clip.txt',
    '-vf', 'fps=15,scale=540:1174:flags=lanczos,format=yuv420p', '-c:v', 'libx264', '-crf', '27', '-preset', 'slow',
    '-movflags', '+faststart', '-an', out], { cwd: args.frames, stdio: 'inherit' });
  if (r.status !== 0) throw new Error('ffmpeg could not make the clip');
  console.log('wrote ' + shown(out) + '  (' + seconds.toFixed(1) + ' s' + (fs.existsSync(walked) ? ', the contact then the tour' : ', the contact alone') + ')');
}

// ---- sweep: transmits ----

async function sweep(args) {
  if (!args.transmit) throw new Error('sweep keys a carrier at every point: pass --transmit, with the control operator at the station.');
  fs.mkdirSync(args.frames, { recursive: true });
  let starts = 0;
  const st = await openStation(args, { landscape: true, holdSite: true,
    allow: (m, p) => { if (p === '/api/swr/start' && starts < 1) { starts++; return true; } return false; } });
  await refuseSweepThroughAmp(st);
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

const commands = { watch, reply, tour, sweep, clip: async (args) => buildClip(args) };

(async () => {
  const args = parseArgs(process.argv.slice(2));
  if (!commands[args.what]) {
    console.error('usage: node webapp/tools/site_shots.mjs watch | tour | clip | reply --transmit | sweep --transmit   (options: the top of this file)');
    process.exit(2);
  }
  await commands[args.what](args);
})().catch((e) => { console.error('site_shots: ' + e.message); process.exit(1); });
