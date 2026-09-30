// Drives sim_station's SWR sweep over HTTP (RFC 0010): the real sweep state machine runs against a
// fake QMX, and the page's /api/swr/start and /api/events see the same JSON the sidekick serves.
// No browser: the page's own rendering is a later slice.

const { spawn } = require('child_process');
const http = require('http');
const path = require('path');

const root = path.join(__dirname, '..', '..');
const bin = path.join(root, 'host_mock', 'sim_station');
const scenario = path.join(root, 'host_mock', 'test_qso.json');
const port = 8792;
const token = 'sim-station-token';

function request(method, pathname, body, withToken) {
  return new Promise((resolve, reject) => {
    const headers = { 'Content-Length': Buffer.byteLength(body || '') };
    if (withToken) headers['X-MicroFT8-Token'] = token;
    const req = http.request({ hostname: '127.0.0.1', port, path: pathname, method, headers, timeout: 2000 }, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        try {
          resolve({ status: res.statusCode, json: JSON.parse(data) });
        } catch (err) {
          reject(err);
        }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => { req.destroy(); reject(new Error('timeout')); });
    req.end(body || '');
  });
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

let failed = false;
function check(ok, msg) {
  if (!ok) {
    console.error('FAIL: ' + msg);
    failed = true;
  }
}

// Sweep events (d == 6) after a cursor, until one satisfies `done` or the time runs out.
async function sweepEvents(since, done, ms) {
  const deadline = Date.now() + ms;
  let seen = [];
  while (Date.now() < deadline) {
    const r = await request('GET', '/api/events?since=' + since);
    seen = r.json.events.filter((e) => e.d === 6);
    if (seen.some(done)) break;
    await sleep(100);
  }
  return seen;
}

async function main() {
  const child = spawn(bin, ['--scale', '50', '--port', String(port), scenario], {
    cwd: path.join(root, 'host_mock'),
    stdio: ['ignore', 'inherit', 'inherit'],
  });
  try {
    for (let i = 0; i < 100; i++) {
      try { await request('GET', '/api/config'); break; } catch (err) { await sleep(100); }
    }

    let r = await request('POST', '/api/swr/start', '7000-7060-20', false);
    check(r.status === 401, 'a start without the token is refused');
    r = await request('POST', '/api/swr/start', 'not a range', true);
    check(r.json.ok === false && r.json.why === 'malformed', 'a malformed body is refused');
    r = await request('POST', '/api/swr/start', '7000-7060-0', true);
    check(r.json.ok === false && r.json.why === 'bad range', 'a zero step is refused');

    // A band the radio lacks (80 m), then 40 m: skipped, four points, done.
    const before = (await request('GET', '/api/events?since=0')).json.seq;
    r = await request('POST', '/api/swr/start', '3500-4000-50,7000-7060-20', true);
    check(r.json.ok === true, 'a sweep starts');
    r = await request('POST', '/api/swr/start', '7000-7060-20', true);
    check(r.json.ok === false && /already running/.test(r.json.why), 'a second start while one runs is refused');
    let ev = await sweepEvents(before, (e) => e.k === 2 || e.k === 3, 20000);
    check(ev.length > 0 && ev[ev.length - 1].k === 2, 'the sweep ends with DONE');
    const skipped = ev.filter((e) => e.k === 1);
    check(skipped.length === 1 && skipped[0].r === 0, '80 m is reported skipped, as range 0');
    const points = ev.filter((e) => e.k === 0);
    check(points.length === 4 && points.every((e) => e.r === 1), 'four points on range 1 (40 m)');
    check(points.map((e) => e.hz).join() === '7000000,7020000,7040000,7060000', 'at 7000, 7020, 7040 and 7060 kHz');
    check(points.every((e) => e.swr >= 100 && e.swr <= 200), 'each a plausible SWR at or below 2 (the half-wave reads low on 40 m)');

    // Stop: a long sweep is cancelled by the same endpoint the page already uses for TX.
    const before2 = (await request('GET', '/api/events?since=0')).json.seq;
    r = await request('POST', '/api/swr/start', '14000-14350-5', true);
    check(r.json.ok === true, 'a long sweep starts');
    await sleep(600);
    await request('POST', '/api/tx/cancel', '', true);
    ev = await sweepEvents(before2, (e) => e.k === 2 || e.k === 3, 20000);
    const last = ev[ev.length - 1];
    check(last && last.k === 3 && last.why === 0, 'a stop reports ABORTED, cancelled');
    check(ev.filter((e) => e.k === 0).length < 71, 'and the sweep did not finish all its points');
    r = await request('POST', '/api/swr/start', '7000-7020-20', true);
    check(r.json.ok === true, 'another sweep starts after a stop');
    await sweepEvents((await request('GET', '/api/events?since=0')).json.seq - 1, (e) => e.k === 2, 20000);
  } finally {
    child.kill('SIGTERM');
  }
  if (!failed) console.log('PASS: sim_station sweep');
  process.exit(failed ? 1 : 0);
}

main();
