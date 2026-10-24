#!/usr/bin/env node
// callinfo.js qrzUrl(): a call with a /suffix or a prefix/ links QRZ's page for the home call.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

// callinfo.js builds its card DOM on load; any element is a stub that accepts every call.
const stub = () => new Proxy(function () {}, { get: () => stub(), apply: () => stub(), set: () => true });
const ctx = vm.createContext({ setInterval() {}, fetch: () => Promise.reject(new Error('offline')), window: {}, document: stub() });
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'callinfo.js'), 'utf8'), ctx);
const qrz = ctx.window.CallInfo.qrzUrl;

let failed = false;
function is(call, want) {
  const got = qrz(call);
  if (got !== 'https://www.qrz.com/db/' + want) { console.error('FAIL: ' + call + ' -> ' + got); failed = true; }
}
is('K1ABC', 'K1ABC');
is('K1ABC/2', 'K1ABC');
is('K1ABC/P', 'K1ABC');
is('K1ABC/QRP', 'K1ABC');
is('PJ4/K1ABC', 'K1ABC');
is('<K1ABC/2>', 'K1ABC');
is('k1abc/m', 'K1ABC');
if (failed) process.exit(1);
console.log('test_callinfo_qrz: ok');
