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
function manifest(ota, p4ota) {
  return {
    commit: 'd18f9b2' + '0'.repeat(33), short: 'd18f9b2', date: '20261110',
    assets: {
      poe_p4: Object.assign({ stable: 'microft8-poe-p4-continuous.bin', sha256: SHA }, p4ota ? { ota: p4ota } : {}),
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
  url: 'http://micro-ft8.kalikstein.com/download/microft8-sidekick-continuous-ota.bin', proto: 14,
}]);
assert.deepStrictEqual(plain(U.offers(manifest(OTA), { sidekick: 'd18f9b2' })), [], 'already on the published build');
// Both boards behind: the main board is offered first, since it is updated first.
const P4OTA = { stable: 'microft8-poe-p4-continuous-ota.bin', filename: 'y.bin', size: 871968, sha256: SHA, version: 'd18f9b2', proto: 15 };
const both = plain(U.offers(manifest(OTA, P4OTA), { sidekick: '7e61488', main: '7e61488' }));
assert.deepStrictEqual(both.map((o) => o.board), ['main', 'sidekick']);
assert.deepStrictEqual(both[0], {
  board: 'main', label: 'Main board', from: '7e61488', to: 'd18f9b2', size: 871968, sha256: SHA,
  url: 'http://micro-ft8.kalikstein.com/download/microft8-poe-p4-continuous-ota.bin', proto: 15,
});
assert.deepStrictEqual(plain(U.offers(manifest(OTA, P4OTA), { sidekick: 'd18f9b2', main: '' })), [],
  'a main board that reports no version (older firmware, or not linked) is offered nothing');
assert.strictEqual(U.startBody(both[0]).split('\n')[0], 'board=main');
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

const eq = (actual, expected, msg) => assert.deepStrictEqual(plain(actual), expected, msg);  // values from the vm are another realm's
// ---- the plan: one request, the main board first ----
const OTA15 = Object.assign({}, OTA, { proto: 15 });
const linked = { hostLinked: true, mainProto: 15, sidekickProto: 15 };
let pl = U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: '7e61488' }, linked);
eq(pl.steps.map((o) => o.board), ['main', 'sidekick']);
assert.strictEqual(pl.blocked, '');
assert.strictEqual(pl.time, 'a few minutes');
// What fw_plan_parse() reads (host_mock/test_fw_policy.cpp): the two requests with a `next` line between.
assert.strictEqual(pl.body, U.startBody(pl.steps[0]) + 'next\n' + U.startBody(pl.steps[1]));
assert.strictEqual(pl.body.split('\n').filter((l) => l === 'next').length, 1);
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: 'd18f9b2' }, linked);
eq([pl.steps.map((o) => o.board), pl.blocked, pl.time, pl.body], [['sidekick'], '', 'about a minute', U.startBody(pl.steps[0])]);
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: 'd18f9b2', main: 'd18f9b2' }, linked);
eq([pl.steps.length, pl.blocked, pl.body], [0, '', '']);
// Pairing. Both boards go to the manifest's protocol: fine whatever they speak now.
assert.strictEqual(U.plan(manifest(OTA15, P4OTA), { sidekick: 'a', main: 'b' }, { hostLinked: true, mainProto: 14, sidekickProto: 14 }).blocked, '');
// Only the sidekick is offered, and its new image speaks a protocol the main board does not.
const OTA16 = Object.assign({}, OTA, { proto: 16 });
assert.strictEqual(U.plan(manifest(OTA16, P4OTA), { sidekick: 'a', main: 'd18f9b2' }, linked).blocked, 'pair');
// Only the main board is offered a build that speaks another protocol than the sidekick stays on.
assert.strictEqual(U.plan(manifest(OTA, Object.assign({}, P4OTA, { proto: 16 })), { sidekick: 'd18f9b2', main: 'b' }, linked).blocked, 'pair');
// No main board answering (a sidekick on the bench by itself), or one too old to say what it speaks: not judged.
assert.strictEqual(U.plan(manifest(OTA16, P4OTA), { sidekick: 'a', main: '' }, { hostLinked: false, mainProto: 0, sidekickProto: 15 }).blocked, '');
assert.strictEqual(U.plan(manifest(OTA16, P4OTA), { sidekick: 'a', main: '' }, { hostLinked: true, mainProto: 0, sidekickProto: 15 }).blocked, '');

// ---- what stopping the radio costs ----
eq(plain(U.stopCosts([])), []);
eq(plain(U.stopCosts(null)), []);
eq(plain(U.stopCosts([
  { dxcall: 'W1AW', state: 1, tries_sent: 0 },
  { dxcall: 'K1ABC', state: 3, tries_sent: 1 },
  { dxcall: 'CQ', state: 0, tries_sent: 4 },
  { dxcall: 'N0CALL', state: 1, tries_sent: 0 },
  { dxcall: 'JA1XYZ', state: 1, tries_sent: 2 },
])), ['Your CQ will end', 'QSO with K1ABC will be abandoned', 'QSO with JA1XYZ will be abandoned', '2 queued replies will be cleared']);
eq(plain(U.stopCosts([{ dxcall: 'W1AW', state: 1, tries_sent: 0 }])), ['1 queued reply will be cleared']);

// ---- the progress screen's steps ----
const PLAN = [{ board: 'main', version: 'd18f9b2', size: 871968 }, { board: 'sidekick', version: 'd18f9b2', size: 1005232 }];
const pst = (o) => Object.assign({ state: 'idle', board: 'main', step: 0, plan: PLAN, got: 0, size: 871968, why: '', at: '' }, o);
const marks = (status, reachable) => U.stepRows(status, reachable !== false).map((r) => r.at[0]).join('');
eq(U.stepRows(pst({ state: 'stopping' }), true).map((r) => r.text), [
  'Stop the radio',
  'Download the main board’s image (852 KB)', 'Verify it', 'Send it to the main board', 'Restart the main board into d18f9b2',
  'Download the sidekick’s image (982 KB)', 'Verify it', 'Restart the sidekick into d18f9b2',
]);
// d done, n now, x next (the first letter of 'next' is n too, so spell them out below).
const ats = (status, reachable) => U.stepRows(status, reachable !== false).map((r) => r.at);
const count = (a, v) => a.filter((x) => x === v).length;
let a = ats(pst({ state: 'stopping' }));
eq([a[0], count(a, 'next')], ['now', 7]);
a = ats(pst({ state: 'transferring', got: 435984 }));
eq(a, ['done', 'done', 'done', 'now', 'next', 'next', 'next', 'next']);
assert.strictEqual(U.stepRows(pst({ state: 'transferring', got: 435984 }), true)[3].text, 'Send it to the main board · 50%');
assert.strictEqual(U.stepRows(pst({ state: 'downloading', got: 87196 }), true)[1].text, 'Download the main board’s image (852 KB) · 9%');
a = ats(pst({ state: 'downloading', step: 1, board: 'sidekick', size: 1005232 }));
eq(a, ['done', 'done', 'done', 'done', 'done', 'now', 'next', 'next']);
// The main board is stopped again before the sidekick's image: shown on that image's first step.
a = ats(pst({ state: 'stopping', step: 1, board: 'sidekick' }));
eq(a, ['done', 'done', 'done', 'done', 'done', 'now', 'next', 'next']);
a = ats(pst({ state: 'restarting', step: 1, board: 'sidekick' }));
eq(a, ['done', 'done', 'done', 'done', 'done', 'done', 'done', 'now']);
// The sidekick stopped answering: its own restart, the plan's last step.
a = ats(pst({ state: 'verifying', step: 1 }), false);
assert.strictEqual(a[7], 'now');
// A failure stays on the step it happened in.
a = ats(pst({ state: 'failed', at: 'transferring', why: 'link' }));
eq(a, ['done', 'done', 'done', 'now', 'next', 'next', 'next', 'next']);
a = ats(pst({ state: 'failed', at: 'stopping', why: 'tx' }));
assert.strictEqual(a[0], 'now');
// A main-board-only plan that finished: everything done.
a = ats(pst({ state: 'done', plan: [PLAN[0]] }));
eq(a, ['done', 'done', 'done', 'done', 'done']);
eq(U.stepRows(null, true).map((r) => r.text), ['Stop the radio']);

assert.strictEqual(U.busy(null), false);
['stopping', 'downloading', 'verifying', 'transferring', 'restarting'].forEach((state) => assert.strictEqual(U.busy({ state }), true, state));
['idle', 'done', 'failed'].forEach((state) => assert.strictEqual(U.busy({ state }), false, state));

// ---- progress ----
const st = (o) => Object.assign({ state: 'idle', board: 'sidekick', version: 'd18f9b2', got: 0, size: 1005232, why: '', detail: 0, running: '7e61488', online: true }, o);
assert.strictEqual(U.progressText(null, true), 'Starting…');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 0 }), true), 'Downloading… 0%');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 432249 }), true), 'Downloading… 42%');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 1005231 }), true), 'Downloading… 99%', 'never 100% before it is all there');
assert.strictEqual(U.progressText(st({ state: 'verifying', got: 1005232 }), true), 'Verifying…');
assert.strictEqual(U.progressText(st({ state: 'restarting' }), true), 'Restarting the sidekick…');
assert.strictEqual(U.progressText(st({ state: 'stopping' }), true), 'Stopping the radio…');
assert.strictEqual(U.progressText(st({ state: 'transferring', board: 'main', got: 435984, size: 871968 }), true), 'Sending to the main board… 50%');
assert.strictEqual(U.progressText(st({ state: 'restarting', board: 'main' }), true), 'Restarting the main board…');
assert.strictEqual(U.progressText(st({ state: 'downloading' }), false), 'Restarting the sidekick…', 'no answer mid-update is the restart');
assert.strictEqual(U.progressText(st({ running: 'd18f9b2', last: { version: 'd18f9b2', result: 'confirming' } }), true), 'Confirming the new version…');

// ---- outcome ----
const last = (result, o) => st(Object.assign({ running: 'd18f9b2', last: { board: 'sidekick', version: 'd18f9b2', from: '7e61488', result } }, o));
assert.strictEqual(U.outcome(null, 'sidekick', 'd18f9b2'), null);
['downloading', 'verifying', 'restarting'].forEach(function (state) {
  assert.strictEqual(U.outcome(st({ state }), 'sidekick', 'd18f9b2'), null, state);
  // A result left by an earlier try at the same version does not end this one.
  assert.strictEqual(U.outcome(last('rolled_back', { state, running: '7e61488' }), 'sidekick', 'd18f9b2'), null, state + ' with an old result');
});
assert.strictEqual(U.outcome(st({}), 'sidekick', 'd18f9b2'), null, 'idle with no result: not started, or not back yet');
assert.strictEqual(U.outcome(last('confirming'), 'sidekick', 'd18f9b2'), null, 'on trial');
assert.deepStrictEqual(plain(U.outcome(last('ok'), 'sidekick', 'd18f9b2')), { ok: true, text: 'Updated to d18f9b2' });
assert.deepStrictEqual(plain(U.outcome(last('rolled_back', { running: '7e61488' }), 'sidekick', 'd18f9b2')),
  { ok: false, text: 'New version did not start — rolled back to 7e61488' });
assert.strictEqual(U.outcome(last('ok'), 'sidekick', 'abc1234'), null, 'a result for some other version');
assert.deepStrictEqual(plain(U.outcome(st({ state: 'failed', why: 'hash' }), 'sidekick', 'd18f9b2')),
  { ok: false, text: 'Verification failed — still on 7e61488' });
assert.deepStrictEqual(plain(U.outcome(st({ state: 'failed', why: 'connect', detail: 202 }), 'sidekick', 'd18f9b2')),
  { ok: false, text: 'Download failed: the sidekick could not reach the download server; check its WiFi has internet — still on 7e61488' });
assert.deepStrictEqual(plain(U.outcome(st({ state: 'failed', why: 'cancelled' }), 'sidekick', 'd18f9b2')),
  { ok: false, text: 'Cancelled — still on 7e61488' });

// A main board's update: the sidekick stays up, so the state says how it ended.
const mst = (o) => st(Object.assign({ board: 'main', size: 871968, host: '7e61488', host_linked: true }, o));
['stopping', 'downloading', 'verifying', 'transferring', 'restarting'].forEach(function (state) {
  assert.strictEqual(U.outcome(mst({ state }), 'main', 'd18f9b2'), null, 'main ' + state);
});
assert.strictEqual(U.outcome(mst({ state: 'idle' }), 'main', 'd18f9b2'), null);
assert.deepStrictEqual(plain(U.outcome(mst({ state: 'done', host: 'd18f9b2' }), 'main', 'd18f9b2')), { ok: true, text: 'Updated to d18f9b2' });
assert.strictEqual(U.outcome(mst({ state: 'done', version: 'abc1234' }), 'main', 'd18f9b2'), null, 'some earlier update’s done');
assert.strictEqual(U.outcome(st({ state: 'done' }), 'main', 'd18f9b2'), null, 'not the main board’s');
assert.deepStrictEqual(plain(U.outcome(mst({ state: 'failed', why: 'rolled_back' }), 'main', 'd18f9b2')),
  { ok: false, text: 'New version did not start — rolled back to 7e61488' });
assert.deepStrictEqual(plain(U.outcome(mst({ state: 'failed', why: 'hash' }), 'main', 'd18f9b2')),
  { ok: false, text: 'Verification failed — still on 7e61488' });
assert.deepStrictEqual(plain(U.outcome(mst({ state: 'failed', why: 'no_slot' }), 'main', 'd18f9b2')),
  { ok: false, text: 'This main board needs one flash over USB before it can update over the air — still on 7e61488' });
assert.deepStrictEqual(plain(U.outcome(mst({ state: 'failed', why: 'no_host', host: '' }), 'main', 'd18f9b2')),
  { ok: false, text: 'The main board is not answering' });

// Every code the sidekick can report has words (sidekick/main/fw_update.c's fail() calls and
// fw_policy.c's fw_start_refusal()).
const fwSrc = ['fw_update.c', 'fw_policy.c'].map((f) => fs.readFileSync(path.join(__dirname, '..', '..', 'sidekick', 'main', f), 'utf8'));
const codes = new Set();
for (const m of fwSrc[0].matchAll(/fail\((?:[^"()]*\?\s*)?"([a-z_]+)"(?:\s*:\s*"([a-z_]+)")?/g)) {
  codes.add(m[1]);
  if (m[2]) codes.add(m[2]);
}
for (const fn of ['const char *fw_start_refusal(', 'const char *fw_transfer_fault(', 'const char *fw_transfer_run(']) {
  const body = fwSrc[1].slice(fwSrc[1].indexOf(fn));
  for (const m of body.slice(0, body.indexOf('\n}\n')).matchAll(/return (?:fault \? fault : )?"([a-z_]+)";/g)) codes.add(m[1]);
}
for (const m of fwSrc[0].matchAll(/refuse\(req, "[^"]+", "([a-z_]+)"\)/g)) codes.add(m[1]);
codes.add('plan');  // fw_plan_parse()'s answer to a plan it will not run, a 400 like a bad field
['body', 'idle'].forEach((c) => codes.delete(c));  // a malformed request and a cancel with nothing running: not an update's end
assert(codes.size >= 22, 'found the sidekick’s codes: ' + [...codes].join(' '));
for (const c of codes) {
  assert(!/^Update failed/.test(U.whyText(c)), 'no words for the sidekick’s "' + c + '"');
}
assert.strictEqual(U.whyText('nonsense'), 'Update failed (nonsense)');
assert.strictEqual(U.whyText(''), 'Update failed');

console.log('ok');
