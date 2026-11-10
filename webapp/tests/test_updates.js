#!/usr/bin/env node
// updates.js (docs/architecture/webapp.md, Updates): what the release manifest offers a board, the
// request handed to the sidekick, and the words for its progress and its result. The manifest here
// is the shape tools/make_manifest.py writes (tools/test_make_manifest.py pins that side).
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const ctx = {};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'updates.js'), 'utf8'), ctx);
const U = vm.runInContext('FwUpdates', ctx);
const plain = (v) => JSON.parse(JSON.stringify(v));

const SHA = 'ba9e8126190833eb2c570b5d08c563ae4e04b1ad3977946633935ed7f8f09539';
function manifest(ota) {
  return {
    commit: 'd18f9b2' + '0'.repeat(33), short: 'd18f9b2', date: '20261110',
    assets: {
      poe_p4: { stable: 'microft8-poe-p4-continuous.bin', sha256: SHA },
      sidekick: Object.assign({ stable: 'microft8-sidekick-continuous.bin', sha256: SHA }, ota === undefined ? {} : { ota }),
    },
  };
}
const OTA = { stable: 'microft8-sidekick-continuous-ota.bin', filename: 'x.bin', size: 1005232, sha256: SHA, version: 'd18f9b2', proto: 14 };

// ---- offers ----
assert.strictEqual(U.MANIFEST_URL, 'https://micro-ft8.kalikstein.com/download/manifest.json');
const offered = plain(U.offers(manifest(OTA), { sidekick: '7e61488' }));
assert.deepStrictEqual(offered, [{
  board: 'sidekick', label: 'Sidekick', from: '7e61488', to: 'd18f9b2', size: 1005232, sha256: SHA,
  // Plain HTTP: the sidekick has no TLS, and the hash is what it checks.
  url: 'http://micro-ft8.kalikstein.com/download/microft8-sidekick-continuous-ota.bin',
}]);
assert.deepStrictEqual(plain(U.offers(manifest(OTA), { sidekick: 'd18f9b2' })), [], 'already on the published build');
assert.strictEqual(U.offers(manifest(OTA), { sidekick: 'd18f9b2-dirty' }).length, 1, 'a changed build is not the published one');
assert.deepStrictEqual(plain(U.offers(manifest(OTA), { sidekick: '' })), [], 'version not known yet');
assert.deepStrictEqual(plain(U.offers(manifest(OTA), {})), []);
assert.deepStrictEqual(plain(U.offers(manifest(undefined), { sidekick: '7e61488' })), [], 'a manifest from before OTA images');
assert.deepStrictEqual(plain(U.offers(null, { sidekick: '7e61488' })), []);
assert.deepStrictEqual(plain(U.offers({}, { sidekick: '7e61488' })), []);
// An entry the sidekick would refuse is not offered.
[
  { stable: '../evil.bin' }, { stable: 'a/b.bin' }, { stable: '' },
  { size: 0 }, { size: 1.5 }, { size: '1005232' },
  { sha256: SHA.toUpperCase() }, { sha256: 'abc' },
  { version: '' }, { version: 'a b' }, { version: 'a"b' }, { version: 'x'.repeat(32) },
].forEach(function (bad) {
  assert.deepStrictEqual(plain(U.offers(manifest(Object.assign({}, OTA, bad)), { sidekick: '7e61488' })), [], JSON.stringify(bad));
});
assert.strictEqual(U.offers(manifest(Object.assign({}, OTA, { version: 'x'.repeat(31) })), { sidekick: '7e61488' }).length, 1);

// ---- the request: the lines fw_request_parse() reads (host_mock/test_fw_policy.cpp) ----
assert.strictEqual(U.startBody(offered[0]),
  'board=sidekick\nurl=http://micro-ft8.kalikstein.com/download/microft8-sidekick-continuous-ota.bin\n' +
  'size=1005232\nsha256=' + SHA + '\nversion=d18f9b2\n');

assert.strictEqual(U.sizeText(1005232), '982 KB');
assert.strictEqual(U.sizeText(839872), '820 KB');
assert.strictEqual(U.sizeText(1048576), '1.0 MB');
assert.strictEqual(U.sizeText(2621440), '2.5 MB');

// ---- progress ----
const st = (o) => Object.assign({ state: 'idle', board: 'sidekick', version: 'd18f9b2', got: 0, size: 1005232, why: '', detail: 0, running: '7e61488', online: true }, o);
assert.strictEqual(U.progressText(null, true), 'Starting…');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 0 }), true), 'Downloading… 0%');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 432249 }), true), 'Downloading… 42%');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 1005231 }), true), 'Downloading… 99%', 'never 100% before it is all there');
assert.strictEqual(U.progressText(st({ state: 'verifying', got: 1005232 }), true), 'Verifying…');
assert.strictEqual(U.progressText(st({ state: 'restarting' }), true), 'Restarting the sidekick…');
assert.strictEqual(U.progressText(st({ state: 'downloading' }), false), 'Restarting the sidekick…', 'no answer mid-update is the restart');
assert.strictEqual(U.progressText(st({ running: 'd18f9b2', last: { version: 'd18f9b2', result: 'confirming' } }), true), 'Confirming the new version…');

// ---- outcome ----
const last = (result, o) => st(Object.assign({ running: 'd18f9b2', last: { board: 'sidekick', version: 'd18f9b2', from: '7e61488', result } }, o));
assert.strictEqual(U.outcome(null, 'd18f9b2'), null);
['downloading', 'verifying', 'restarting'].forEach(function (state) {
  assert.strictEqual(U.outcome(st({ state }), 'd18f9b2'), null, state);
  // A result left by an earlier try at the same version does not end this one.
  assert.strictEqual(U.outcome(last('rolled_back', { state, running: '7e61488' }), 'd18f9b2'), null, state + ' with an old result');
});
assert.strictEqual(U.outcome(st({}), 'd18f9b2'), null, 'idle with no result: not started, or not back yet');
assert.strictEqual(U.outcome(last('confirming'), 'd18f9b2'), null, 'on trial');
assert.deepStrictEqual(plain(U.outcome(last('ok'), 'd18f9b2')), { ok: true, text: 'Updated to d18f9b2' });
assert.deepStrictEqual(plain(U.outcome(last('rolled_back', { running: '7e61488' }), 'd18f9b2')),
  { ok: false, text: 'New version did not start — rolled back to 7e61488' });
assert.strictEqual(U.outcome(last('ok'), 'abc1234'), null, 'a result for some other version');
assert.deepStrictEqual(plain(U.outcome(st({ state: 'failed', why: 'hash' }), 'd18f9b2')),
  { ok: false, text: 'Verification failed — still on 7e61488' });
assert.deepStrictEqual(plain(U.outcome(st({ state: 'failed', why: 'dns', detail: 202 }), 'd18f9b2')),
  { ok: false, text: 'Download failed: the sidekick’s WiFi has no internet — still on 7e61488' });
assert.deepStrictEqual(plain(U.outcome(st({ state: 'failed', why: 'cancelled' }), 'd18f9b2')),
  { ok: false, text: 'Cancelled — still on 7e61488' });

// Every code the sidekick can report has words (sidekick/main/fw_update.c's fail() calls and
// fw_policy.c's fw_start_refusal()).
const fwSrc = ['fw_update.c', 'fw_policy.c'].map((f) => fs.readFileSync(path.join(__dirname, '..', '..', 'sidekick', 'main', f), 'utf8'));
const codes = new Set();
for (const m of fwSrc[0].matchAll(/fail\((?:[^"()]*\?\s*)?"([a-z_]+)"(?:\s*:\s*"([a-z_]+)")?/g)) {
  codes.add(m[1]);
  if (m[2]) codes.add(m[2]);
}
const refusal = fwSrc[1].slice(fwSrc[1].indexOf('const char *fw_start_refusal('));
for (const m of refusal.slice(0, refusal.indexOf('\n}\n')).matchAll(/return "([a-z_]+)";/g)) codes.add(m[1]);
assert(codes.size >= 12, 'found the sidekick’s codes: ' + [...codes].join(' '));
for (const c of codes) {
  assert(!/^Update failed/.test(U.whyText(c)), 'no words for the sidekick’s "' + c + '"');
}
assert.strictEqual(U.whyText('nonsense'), 'Update failed (nonsense)');
assert.strictEqual(U.whyText(''), 'Update failed');

console.log('ok');
