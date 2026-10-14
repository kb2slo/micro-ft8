// Drives sim_station over HTTP and checks the scripted QSO shows up as the
// same event JSON the web app polls, then drives the CQ row's routes (Call CQ,
// tries and parity in place, the x) and checks the queue rows they produce.
// No browser: the page's own rendering of these rows is test_decode_batching.js.

const { spawn } = require('child_process');
const http = require('http');
const path = require('path');

const root = path.join(__dirname, '..', '..');
const bin = path.join(root, 'host_mock', 'sim_station');
const scenario = path.join(root, 'host_mock', 'test_qso.json');
const port = 8791;

function get(pathname) {
  return new Promise((resolve, reject) => {
    const req = http.get({ hostname: '127.0.0.1', port, path: pathname, timeout: 2000 }, (res) => {
      let body = '';
      res.on('data', (chunk) => { body += chunk; });
      res.on('end', () => {
        try {
          resolve(JSON.parse(body));
        } catch (err) {
          reject(err);
        }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => {
      req.destroy();
      reject(new Error('timeout'));
    });
  });
}

function post(pathname, body, token) {
  return new Promise((resolve, reject) => {
    const data = body === undefined ? '' : String(body);
    const req = http.request({
      hostname: '127.0.0.1', port, path: pathname, method: 'POST', timeout: 2000,
      headers: { 'x-microft8-token': token, 'content-length': Buffer.byteLength(data) },
    }, (res) => {
      let text = '';
      res.on('data', (chunk) => { text += chunk; });
      res.on('end', () => {
        try {
          resolve({ status: res.statusCode, body: JSON.parse(text) });
        } catch (err) {
          reject(err);
        }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => {
      req.destroy();
      reject(new Error('timeout'));
    });
    req.end(data);
  });
}

// The latest QUEUE_ENTRY row for each entry, from the whole event log.
async function queueRows() {
  const events = await get('/api/events?since=0');
  const rows = {};
  for (const e of events.events || []) {
    if (e.d === 2) rows[e.eid] = e;
  }
  return rows;
}

async function cqRow() {
  const rows = await queueRows();
  return Object.values(rows).find((e) => e.x === 'CQ' && e.st !== 6) || null;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function main() {
  const child = spawn(bin, ['--scale', '50', '--port', String(port), scenario], {
    cwd: path.join(root, 'host_mock'),
    stdio: ['ignore', 'inherit', 'inherit'],
  });
  let failed = false;
  try {
    const deadline = Date.now() + 15000;
    let sawDecode = false;
    let sawTx = false;
    while (Date.now() < deadline && child.exitCode == null) {
      try {
        const events = await get('/api/events?since=0');
        const text = JSON.stringify(events);
        sawDecode = text.includes('CQ N6HAN CM87');
        sawTx = text.includes('N6HAN AG6AQ CM97');
        if (sawDecode && sawTx) {
          break;
        }
      } catch (err) {
        // The process is still binding.
      }
      await sleep(100);
    }
    if (!sawDecode) {
      console.error('missing scripted decode CQ N6HAN CM87');
      failed = true;
    }
    if (!sawTx) {
      console.error('missing our TX N6HAN AG6AQ CM97');
      failed = true;
    }
    if (!failed) {
      failed = !(await checkCqRoutes());
    }
    if (!failed) {
      console.log('PASS: sim_station events and the CQ row');
    }
  } finally {
    child.kill('SIGTERM');
  }
  process.exit(failed ? 1 : 0);
}

// The CQ row's routes (docs/architecture/webapp.md, The CQ row).
async function checkCqRoutes() {
  const ok = (cond, what) => {
    if (!cond) console.error('FAIL: ' + what);
    return cond;
  };
  const token = (await get('/api/pairing-token')).token;
  let pass = true;
  pass = ok((await post('/api/cq', undefined, token)).body.ok === true, 'Call CQ') && pass;
  let cq = await cqRow();
  pass = ok(cq && cq.st === 0 && cq.tl === 6 && cq.cu === 0, 'a CQ row with the default 6 tries') && pass;
  if (!cq) return false;
  const again = await post('/api/cq', undefined, token);
  pass = ok(again.body.ok === false, 'a second Call CQ is refused') && pass;

  pass = ok((await post('/api/queue/tries', cq.eid + ' 12', token)).body.ok === true, 'tries to 12') && pass;
  cq = await cqRow();
  pass = ok(cq.tl === 12 && cq.cu === 1, 'the CQ row reads 12, custom') && pass;
  pass = ok((await post('/api/queue/tries', cq.eid + ' 100', token)).status === 400, 'not 100') && pass;

  const flipped = cq.p ? 0 : 1;
  pass = ok((await post('/api/queue/parity', cq.eid + ' ' + flipped, token)).body.ok === true, 'flip') && pass;
  cq = await cqRow();
  pass = ok(cq.p === flipped, 'the CQ moved to the other period') && pass;

  // Empty periods run after the script, so the CQ goes out and counts.
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline && (cq = await cqRow()) && cq.ts === 0) await sleep(100);
  pass = ok(cq && cq.ts > 0, 'the CQ goes out and counts a try') && pass;

  pass = ok((await post('/api/queue/cancel', String(cq.eid), token)).body.ok === true, 'x on the CQ row') && pass;
  pass = ok((await cqRow()) === null, 'no CQ queued after x') && pass;
  return pass;
}

main();
