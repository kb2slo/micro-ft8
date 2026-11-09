#!/usr/bin/env node
// callinfo.js learnCq() / heardCqFor() (docs/architecture/webapp.md, the callsign card): a decode's CQ POTA or
// CQ SOTA is kept by call for the UTC day, with its band and mode, newest sighting winning and the oldest dropped
// past the cap; anything else is ignored; and app.html offers every decode to it.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const stub = () => new Proxy(function () {}, { get: () => stub(), apply: () => stub(), set: () => true });
const store = {};
const localStorage = {
  getItem: (k) => (k in store ? store[k] : null),
  setItem: (k, v) => { store[k] = String(v); },
  removeItem: (k) => { delete store[k]; },
};
const ctx = vm.createContext({ setInterval() {}, fetch: () => Promise.reject(new Error('offline')), window: {},
                               document: stub(), localStorage });
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'callinfo.js'), 'utf8'), ctx);
const CI = ctx.window.CallInfo;

const day = Date.UTC(2026, 10, 9, 13, 0, 0);
CI.learnCq('CQ POTA K1ABC FN42', 14074, 'FT8', day);
let h = CI.heardCqFor('k1abc', day + 1000);
assert.deepStrictEqual(h && [h.program, h.band, h.mode, h.ts], ['POTA', '20m', 'FT8', day], 'a CQ POTA is kept with band and mode');

CI.learnCq('CQ SOTA W7XYZ/P', 7047.5, 'FT4', day + 60000);
h = CI.heardCqFor('W7XYZ/P', day + 61000);
assert.deepStrictEqual(h && [h.program, h.band, h.mode], ['SOTA', '40m', 'FT4'], 'a CQ SOTA with no grid, a portable call');

CI.learnCq('CQ POTA K1ABC FN42', 7074, 'FT8', day + 120000);
h = CI.heardCqFor('K1ABC', day + 121000);
assert.deepStrictEqual(h && [h.band, h.ts], ['40m', day + 120000], 'the newest sighting wins');

for (const x of ['CQ K2DEF FN31', 'CQ DX K2DEF FN31', 'K1ABC K2DEF FN31', 'CQ POTA', 'CQ POTA <...> FN42', 'TNX POTA K2DEF']) {
  CI.learnCq(x, 14074, 'FT8', day);
}
assert.strictEqual(CI.heardCqFor('K2DEF', day), null, 'anything but a CQ POTA/SOTA is ignored');

assert.strictEqual(CI.heardCqFor('K1ABC', Date.UTC(2026, 10, 10, 0, 0, 1)), null, 'kept for the UTC day only');

for (let i = 0; i < 510; i++) CI.learnCq('CQ POTA K' + i + 'AA FN42', 14074, 'FT8', day + 200000 + i);
assert.strictEqual(CI.heardCqFor('K0AA', day + 300000), null, 'past the cap the oldest go');
assert.ok(CI.heardCqFor('K509AA', day + 300000), 'and the newest stay');
assert.ok(Object.keys(JSON.parse(store.microft8_cq_heard_v1).calls).length <= 500, 'at most 500 calls');

const app = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');
assert(/learnCallsignGrid\(e\.x\);[^\n]*\n[^\n]*\n\s*if \(typeof CallInfo !== 'undefined'\) CallInfo\.learnCq\(e\.x,/.test(app),
  'every decode is offered to learnCq()');

console.log('test_callinfo_cq: ok');
