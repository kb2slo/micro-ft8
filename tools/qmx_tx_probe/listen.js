#!/usr/bin/env node
// A call's spots from PSKReporter's live MQTT feed, through the web app's own listener (pskr_core.js,
// pskr_live.js, unchanged) with a real WebSocket, as PSKReporter asks clients to read it (docs/architecture/system.md). Prints each new spot: time sent, mode, RF Hz, receiver, SNR.
'use strict';
const vm = require('vm'), fs = require('fs'), path = require('path');
const APP = path.join(__dirname, '..', '..', 'webapp', 'app');
const CALL = (process.argv[2] || '').toUpperCase();
if (!CALL) { console.error('usage: node listen.js <CALL>'); process.exit(2); }
const store = {};
const noop = () => {};
const ctx = vm.createContext({
  WebSocket, Date, Math, JSON, Uint8Array, ArrayBuffer, TextDecoder, TextEncoder, console,
  setTimeout, setInterval, clearTimeout, clearInterval,
  localStorage: { getItem: (k) => store[k] ?? null, setItem: (k, v) => { store[k] = String(v); }, removeItem: (k) => { delete store[k]; } },
  document: { visibilityState: 'visible', addEventListener: noop },
  window: { addEventListener: noop }, navigator: { onLine: true },
});
for (const f of ['pskr_core.js', 'pskr_live.js']) vm.runInContext(fs.readFileSync(path.join(APP, f), 'utf8'), ctx, { filename: f });
const seen = new Set();
const stamp = (ms) => new Date(ms).toLocaleTimeString('en-US', { hour12: false });
vm.runInContext('this.L = PskrLive', ctx);
ctx.L.start({
  call: () => CALL,
  want: () => true,
  changed: (what) => {
    if (what === 'status') console.log(stamp(Date.now()), 'status', ctx.L.status());
    if (what !== 'spots') return;
    for (const s of ctx.L.state().spots.slice().reverse()) {
      const k = s.rx + '|' + s.mode + '|' + s.t;
      if (seen.has(k)) continue;
      seen.add(k);
      console.log(stamp(s.t), s.mode, s.hz, s.rx, s.grid, s.snr, 'heard at', stamp(Date.now()));
    }
  },
});
