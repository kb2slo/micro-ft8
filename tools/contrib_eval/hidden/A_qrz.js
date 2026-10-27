#!/usr/bin/env node
// Task A's hidden check, run by grade.py against a run's callinfo.js (argv[2]): a call with a /suffix or a
// prefix/ links QRZ's page for the home call. It asks the card itself, through show(), so a fix is graded
// on what it does and not on what its function is called.
'use strict';
const vm = require('vm');
const fs = require('fs');

const stub = () => new Proxy(function () {}, { get: () => stub(), apply: () => stub(), set: () => true });
const qrzLink = {};
const document = new Proxy(stub(), {
  get(target, key) {
    if (key === 'getElementById') return (id) => (id === 'callInfoQrz' ? qrzLink : stub());
    return stub();
  },
});
const ctx = vm.createContext({ setInterval() {}, setTimeout() {}, fetch: () => Promise.reject(new Error('offline')),
                              window: {}, document, localStorage: stub(), navigator: stub(), console });
vm.runInContext(fs.readFileSync(process.argv[2], 'utf8'), ctx);

let failed = false;
function is(call, want) {
  qrzLink.href = undefined;
  try { ctx.window.CallInfo.show(call); } catch (e) { console.error('show(' + call + ') threw: ' + e.message); }
  const got = qrzLink.href;
  if (got !== 'https://www.qrz.com/db/' + want) { console.error('FAIL: ' + call + ' -> ' + got); failed = true; }
}
is('K1ABC', 'K1ABC');
is('K1ABC/2', 'K1ABC');
is('K1ABC/P', 'K1ABC');
is('K1ABC/QRP', 'K1ABC');
is('PJ4/K1ABC', 'K1ABC');
is('k1abc/m', 'K1ABC');
if (failed) process.exit(1);
console.log('hidden A_qrz: ok');
