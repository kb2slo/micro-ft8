// Drives sim_station over HTTP and checks the scripted QSO shows up as the
// same event JSON the web app polls. No browser: the page's own rendering
// of these rows is test_decode_batching.js.

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
      console.log('PASS: sim_station events');
    }
  } finally {
    child.kill('SIGTERM');
  }
  process.exit(failed ? 1 : 0);
}

main();
