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
const eq0 = (actual, expected, msg) => assert.deepStrictEqual(plain(actual), expected, msg);

const SHA = 'ba9e8126190833eb2c570b5d08c563ae4e04b1ad3977946633935ed7f8f09539';
// The published dev build, as tools/build_version.py names it, and the release cut at its commit.
const V = 'b1322+d18f9b2';
const REL = '0.1.0+d18f9b2';
function manifest(ota, p4ota) {
  return {
    channel: 'dev', commit: 'd18f9b2' + '0'.repeat(33), short: 'd18f9b2', version: V, build: 1322, date: '20261110',
    assets: {
      poe_p4: Object.assign({ stable: 'microft8-poe-p4-dev.bin', sha256: SHA }, p4ota ? { ota: p4ota } : {}),
      sidekick: Object.assign({ stable: 'microft8-sidekick-dev.bin', sha256: SHA }, ota === undefined ? {} : { ota }),
    },
  };
}
const OTA = { stable: 'microft8-sidekick-dev-ota.bin', filename: '20261110-microft8-sidekick-ota-d18f9b2.bin', size: 1005232, sha256: SHA, version: V, proto: 14 };

// ---- offers ----
eq0(U.MANIFEST_URLS, { dev: 'https://micro-ft8.kalikstein.com/download/manifest.json',
  stable: 'https://micro-ft8.kalikstein.com/download/manifest-stable.json' });
const offered = plain(U.offers(manifest(OTA), { sidekick: '7e61488' }));
assert.deepStrictEqual(offered, [{
  board: 'sidekick', label: 'Sidekick', from: '7e61488', to: V, size: 1005232, sha256: SHA,
  // Plain HTTP: the sidekick has no TLS, and the hash is what it checks. The dated name, which a
  // later publish never overwrites, not the stable one, so a publish mid-update changes nothing.
  url: 'http://micro-ft8.kalikstein.com/download/ota/20261110-microft8-sidekick-ota-d18f9b2.bin', proto: 14,
}]);
assert.deepStrictEqual(plain(U.offers(manifest(OTA), { sidekick: V })), [], 'already on the published build');
// Both boards behind: the main board is offered first, since it is updated first.
const P4OTA = { stable: 'microft8-poe-p4-dev-ota.bin', filename: '20261110-microft8-poe-p4-ota-d18f9b2.bin', size: 871968, sha256: SHA, version: V, proto: 15 };
const both = plain(U.offers(manifest(OTA, P4OTA), { sidekick: '7e61488', main: '7e61488' }));
assert.deepStrictEqual(both.map((o) => o.board), ['main', 'sidekick']);
assert.deepStrictEqual(both[0], {
  board: 'main', label: 'Main board', from: '7e61488', to: V, size: 871968, sha256: SHA,
  url: 'http://micro-ft8.kalikstein.com/download/ota/20261110-microft8-poe-p4-ota-d18f9b2.bin', proto: 15,
});
assert.deepStrictEqual(plain(U.offers(manifest(OTA, P4OTA), { sidekick: V, main: '' })), [],
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
  { filename: '../evil.bin' }, { filename: 'a/b.bin' }, { filename: '' }, { filename: '.hidden' }, { filename: undefined },
  { size: 0 }, { size: 1.5 }, { size: '1005232' },
  { sha256: SHA.toUpperCase() }, { sha256: 'abc' },
  { version: '' }, { version: 'a b' }, { version: 'a"b' }, { version: 'x'.repeat(32) },
  // Not a dev build's version: a bench build's, a changed tree's, a release in the dev manifest.
  { version: 'd18f9b2' }, { version: V + '-dirty' }, { version: REL },
].forEach(function (bad) {
  assert.deepStrictEqual(plain(U.offers(manifest(Object.assign({}, OTA, bad)), { sidekick: '7e61488' })), [], JSON.stringify(bad));
});

// ---- versions and channels (tools/build_version.py writes them; tools/test_build_version.py) ----
eq0(U.parseVersion(V), { channel: 'dev', label: 'b1322', sha: 'd18f9b2', num: null, build: 1322 });
eq0(U.parseVersion(REL), { channel: 'stable', label: '0.1.0', sha: 'd18f9b2', num: [0, 1, 0], build: 0 });
['d18f9b2', 'd18f9b2-dirty', V + '-dirty', REL + '-dirty', 'b0+d18f9b2', 'b1322', '0.1.0', '1322+d18f9b2', 'v0.1.0+d18f9b2',
  'b1322+D18F9B2', 'b1322+d18f9b', '0.1+d18f9b2', '01.0.0+d18f9b2', 'unknown', '', undefined, null, 7].forEach((v) =>
  assert.strictEqual(U.parseVersion(v), null, String(v)));
eq0([U.versionLabel(V), U.versionLabel(REL), U.versionLabel('7e61488-dirty'), U.versionLabel(''), U.versionLabel(undefined)],
  ['b1322', '0.1.0', '7e61488-dirty', '', '']);
eq0([U.versionText(V), U.versionText(REL), U.versionText('7e61488'), U.versionText('')], ['b1322 (d18f9b2)', '0.1.0 (d18f9b2)', '7e61488', '']);
// The channel is the main board's: a release is stable, anything else dev.
eq0([U.channelOf({ main: REL, sidekick: V }), U.channelOf({ main: V, sidekick: REL }), U.channelOf({ main: '7e61488' }),
  U.channelOf({ sidekick: REL }), U.channelOf({}), U.channelOf(null)], ['stable', 'dev', 'dev', 'dev', 'dev', 'dev']);
eq0([U.channelName('stable'), U.channelName('dev'), U.channelName('x')], ['Stable', 'Dev', '']);

// ---- a channel offers only a later build ----
const offeredTo = (m, running, sw) => plain(U.offers(m, running, sw)).map((o) => o.board);
eq0(offeredTo(manifest(OTA, P4OTA), { main: 'b1321+7e61488', sidekick: 'b1321+7e61488' }), ['main', 'sidekick'], 'an earlier dev build');
eq0(offeredTo(manifest(OTA, P4OTA), { main: 'b1323+abc1234', sidekick: 'b1323+abc1234' }), [], 'a later dev build is never offered an older one');
eq0(offeredTo(manifest(OTA, P4OTA), { main: 'b1322+abc1234', sidekick: V }), [], 'the same number is not later');
eq0(offeredTo(manifest(OTA, P4OTA), { main: 'b99999+abc1234', sidekick: 'b999+7e61488' }), ['sidekick'], 'by number, not as text');
eq0(offeredTo(manifest(OTA, P4OTA), { main: '7e61488', sidekick: 'b1323+abc1234-dirty' }), ['main', 'sidekick'], 'a bench build is on no channel');
// The stable manifest, in the same form.
function stable(number, build) {
  const v = number + '+d18f9b2';
  const m = manifest(Object.assign({}, OTA, { version: v, filename: 'microft8-sidekick-ota-' + number + '.bin' }),
    Object.assign({}, P4OTA, { version: v, filename: 'microft8-poe-p4-ota-' + number + '.bin' }));
  return Object.assign(m, { channel: 'stable', version: v, build: build });
}
eq0(offeredTo(stable('0.10.0', 1322), { main: '0.9.1+7e61488', sidekick: '0.9.1+7e61488' }), ['main', 'sidekick'], 'the next release');
eq0(offeredTo(stable('0.10.0', 1322), { main: '0.10.1+abc1234', sidekick: '1.0.0+abc1234' }), [], 'never an earlier release');
eq0(plain(U.offers(stable('0.1.0', 1322), { main: '0.0.9+7e61488' }))[0].url,
  'http://micro-ft8.kalikstein.com/download/ota/microft8-poe-p4-ota-0.1.0.bin');
// A half-switched station: the main board is on the release, the sidekick still on dev. It reads
// as stable, and the stable channel's Update finishes it.
eq0(offeredTo(stable('0.1.0', 1322), { main: REL, sidekick: 'b1330+abc1234' }), ['sidekick']);
// A manifest never offers another channel's image, or anything without saying its channel.
eq0(offeredTo(Object.assign(manifest(OTA, P4OTA), { channel: 'stable' }), { main: '7e61488', sidekick: '7e61488' }), []);
eq0(offeredTo(Object.assign(manifest(OTA, P4OTA), { channel: undefined }), { main: '7e61488', sidekick: '7e61488' }), []);
// Switching: every board that differs, an older build included.
eq0(offeredTo(stable('0.1.0', 1322), { main: 'b1330+abc1234', sidekick: 'b1330+abc1234' }, true), ['main', 'sidekick']);
eq0(offeredTo(manifest(OTA, P4OTA), { main: REL, sidekick: REL }, true), ['main', 'sidekick']);
eq0(offeredTo(manifest(OTA, P4OTA), { main: V, sidekick: REL }, true), ['sidekick']);

// ---- the request: the lines fw_request_parse() reads (host_mock/test_fw_policy.cpp) ----
assert.strictEqual(U.startBody(offered[0]),
  'board=sidekick\nurl=http://micro-ft8.kalikstein.com/download/ota/20261110-microft8-sidekick-ota-d18f9b2.bin\n' +
  'size=1005232\nsha256=' + SHA + '\nversion=' + V + '\n');

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
assert.strictEqual(pl.time, 'about three minutes');
assert.strictEqual(U.plan(manifest(OTA15, P4OTA), { sidekick: V, main: '7e61488' }, linked).time, 'about two minutes');
// What fw_plan_parse() reads (host_mock/test_fw_policy.cpp): the two requests with a `next` line between.
assert.strictEqual(pl.body, U.startBody(pl.steps[0]) + 'next\n' + U.startBody(pl.steps[1]));
assert.strictEqual(pl.body.split('\n').filter((l) => l === 'next').length, 1);
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: V }, linked);
eq([pl.steps.map((o) => o.board), pl.blocked, pl.time, pl.body], [['sidekick'], '', 'about a minute', U.startBody(pl.steps[0])]);
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: V, main: V }, linked);
eq([pl.steps.length, pl.blocked, pl.body], [0, '', '']);
// Pairing. Both boards go to the manifest's protocol: fine whatever they speak now.
assert.strictEqual(U.plan(manifest(OTA15, P4OTA), { sidekick: 'a', main: 'b' }, { hostLinked: true, mainProto: 14, sidekickProto: 14 }).blocked, '');
// Only the sidekick is offered, and its new image speaks a protocol the main board does not.
const OTA16 = Object.assign({}, OTA, { proto: 16 });
assert.strictEqual(U.plan(manifest(OTA16, P4OTA), { sidekick: 'a', main: V }, linked).blocked, 'pair');
// Only the main board is offered a build that speaks another protocol than the sidekick stays on.
assert.strictEqual(U.plan(manifest(OTA, Object.assign({}, P4OTA, { proto: 16 })), { sidekick: V, main: 'b' }, linked).blocked, 'pair');
// No main board answering (a sidekick on the bench by itself), or one too old to say what it speaks: not judged.
assert.strictEqual(U.plan(manifest(OTA16, P4OTA), { sidekick: 'a', main: '' }, { hostLinked: false, mainProto: 0, sidekickProto: 15 }).blocked, '');
assert.strictEqual(U.plan(manifest(OTA16, P4OTA), { sidekick: 'a', main: '' }, { hostLinked: true, mainProto: 0, sidekickProto: 15 }).blocked, '');

// Nothing about a radio blocks a plan: the review asks for it off, and nothing checks.
eq(U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: '7e61488' }, Object.assign({ radioAttached: true }, linked)).blocked, '');

// ---- the plan has no app step ----
// The web app is part of the sidekick's image: a station on the published firmware has nothing to
// update, however its page was built, and no plan carries an app.
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: V, main: V }, linked);
eq([pl.any, pl.steps.length, pl.body, 'app' in pl], [false, 0, '', false]);
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: '7e61488' }, linked);
eq([pl.any, pl.steps.map((o) => o.board), pl.time, 'app' in pl], [true, ['main', 'sidekick'], 'about three minutes', false]);
// A manifest that still names an app (a release cut before the bundle's publishing stops) adds nothing.
pl = U.plan(Object.assign(stable('0.1.0', 1322), { app: { path: 'app/0.1.0/', build: REL + ' 2026-11-13', size: 185745 } }),
  { sidekick: REL, main: REL }, linked);
eq([pl.any, pl.steps.length], [false, 0]);
// The body is the boards' images and nothing else: the main board's, then the sidekick's.
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: '7e61488' }, linked);
eq(pl.body.split('\n').filter((l) => /^board=|^next$/.test(l)), ['board=main', 'next', 'board=sidekick']);
// Unavailable: no main board (the boards update together), or the sidekick on its own network.
eq(U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: '' }, { hostLinked: false, sidekickProto: 15 }).unavailable, 'no_host');
eq(U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: '7e61488' }, Object.assign({ ap: true }, linked)).unavailable, 'ap');
assert(/main board/.test(U.unavailableText('no_host')) && /own network/.test(U.unavailableText('ap')));
assert.strictEqual(U.unavailableText(''), '');

// The review: says once that nothing transmits, names nothing it ends (the band change's dialog
// before it did), asks for the radio off, says the phone may lock, since no step is the page's, and
// never promises receiving goes on.
let rv = U.reviewText(pl);
eq(rv, ['Takes about three minutes. Nothing transmits until the update is done.',
  'Switch the radio off, or unplug its USB, before you update. If it stays connected it may need switching off and on afterwards.',
  'The update goes on if this phone locks.']);
rv = U.reviewText(U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: V }, linked));
eq([rv[0], rv[2]], ['Takes about a minute. Nothing transmits until the update is done.', 'The update goes on if this phone locks.']);
rv.forEach((t) => assert(!/receiv|decod|page open/i.test(t), 'no promise about receiving, and no page to keep open: ' + t));

// The note under the offer: the radio off, as the review asks, or why the plan cannot run.
eq(U.offerNote(U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: '7e61488' }, linked)), 'Switch the radio off first.');
eq(U.offerNote(U.plan(manifest(OTA15, P4OTA), { sidekick: '7e61488', main: V }, linked)), 'Switch the radio off first.');
eq(U.offerNote(U.plan(manifest(OTA16, P4OTA), { sidekick: 'a', main: V }, linked)), U.whyText('pair') + '.');

// ---- the plan's channel, and a switch of channel ----
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: 'b1321+7e61488', main: 'b1321+7e61488' }, linked);
eq([pl.channel, pl.target, pl.switching, pl.older, pl.to, pl.steps.length], ['dev', 'dev', false, false, 'b1322', 2]);
eq(U.reviewText(pl)[0], 'Takes about three minutes. Nothing transmits until the update is done.', 'an update says nothing of channels');
// Dev to stable, to a release cut from an earlier commit than the build the station runs.
const dev1330 = { sidekick: 'b1330+abc1234', main: 'b1330+abc1234' };
eq(U.plan(stable('0.1.0', 1322), dev1330, linked).steps.length, 2, 'not on stable, so its Update would move it: the page asks as a switch');
pl = U.plan(stable('0.1.0', 1322), dev1330, linked, true);
eq([pl.channel, pl.target, pl.switching, pl.older, pl.to, pl.steps.map((o) => o.board), pl.steps[0].to],
  ['dev', 'stable', true, true, '0.1.0', ['main', 'sidekick'], REL]);
eq(U.switchLabel(pl), 'Switch to Stable (0.1.0)');
rv = U.reviewText(pl);
eq(rv[0], 'This station moves to the Stable channel, 0.1.0. That is an older build than the one it runs.');
eq(rv[1], 'Takes about three minutes. Nothing transmits until the update is done.');
assert(/Switch the radio off/.test(rv[2]));
// The same code or later: not called older. A bench build's age is not known, and not guessed.
eq(U.plan(stable('0.1.0', 1322), { sidekick: V, main: V }, linked, true).older, false);
eq(U.plan(stable('0.1.0', 1330), { sidekick: V, main: V }, linked, true).older, false);
eq(U.plan(stable('0.1.0', 1322), { sidekick: '7e61488', main: '7e61488' }, linked, true).older, false);
// Stable to dev: dev is built from main, at or past any release.
pl = U.plan(manifest(OTA15, P4OTA), { sidekick: REL, main: REL }, linked, true);
eq([pl.channel, pl.target, pl.switching, pl.older, U.switchLabel(pl), U.reviewText(pl)[0]],
  ['stable', 'dev', true, false, 'Switch to Dev (b1322)', 'This station moves to the Dev channel, b1322.']);
// A switch is held to the same pairing rule as an update.
eq(U.plan(stable('0.1.0', 1322), { sidekick: REL, main: 'b1330+abc1234' }, Object.assign({}, linked, { sidekickProto: 14 }), true).blocked, 'pair');
// No manifest for the other channel (no release cut yet): nothing to switch to.
pl = U.plan(null, dev1330, linked, true);
eq([pl.any, pl.target, pl.to, pl.steps.length], [false, 'dev', '', 0]);

// ---- the app's relay, against a fake host and sidekick ----
function relayWorld(o) {
  o = o || {};
  const files = { 'app.html': 'A'.repeat(10), 'updates.js': 'B'.repeat(5) };
  const man = { api_min: 3, assets: Object.keys(files).map((p) => ({ path: p, sha256: '0'.repeat(64), size: files[p].length })) };
  const calls = [];
  const res = (status, body) => ({ ok: status >= 200 && status < 300, status,
    text: async () => body || '', arrayBuffer: async () => new TextEncoder().encode(body || '').buffer });
  const io = {
    get: async (url) => {
      calls.push('get ' + url);
      if (o.offline) throw new TypeError('Load failed');
      if (url.endsWith('/manifest.json')) return res(200, JSON.stringify(man));
      const name = url.split('/').pop();
      return res(200, o.grown === name ? files[name] + 'x' : files[name]);
    },
    begin: async (text) => { calls.push('begin'); assert.deepStrictEqual(JSON.parse(text), man, 'the manifest posted as fetched'); return o.begin || res(200); },
    put: async (path, bytes) => { calls.push('put ' + path + ' ' + bytes.byteLength); return o.put || res(200); },
    commit: async () => { calls.push('commit'); return o.commit || res(200); },
    progress: (done, total) => calls.push('progress ' + done + '/' + total),
  };
  return { io, calls, res };
}
(async function () {
  let w = relayWorld();
  eq(await U.relayApp('https://h.example/app/', w.io), { ok: true });
  eq(w.calls, ['get https://h.example/app/manifest.json', 'begin', 'progress 0/15',
    'get https://h.example/app/app.html', 'put app.html 10', 'progress 10/15',
    'get https://h.example/app/updates.js', 'put updates.js 5', 'progress 15/15', 'commit']);
  // Firmware too old for this app: refused at begin, nothing staged; the plan runs the firmware first.
  w = relayWorld(); w.io.begin = async () => w.res(400, 'api_min 3 requires firmware API 3 (have 2)');
  eq((await U.relayApp('https://h.example/app', w.io)).why, 'firmware');
  // Published again mid-relay: a size the manifest does not name, or a hash the sidekick refuses.
  w = relayWorld({ grown: 'updates.js' });
  eq((await U.relayApp('https://h.example/app', w.io)).why, 'changed');
  assert(!w.calls.includes('commit') && !w.calls.some((c) => c.startsWith('put updates.js')), 'nothing promoted');
  w = relayWorld(); w.io.put = async () => w.res(400, 'sha256 mismatch');
  eq((await U.relayApp('https://h.example/app', w.io)).why, 'changed');
  // Not paired, the host out of reach, anything else refused.
  w = relayWorld(); w.io.begin = async () => w.res(401);
  eq((await U.relayApp('https://h.example/app', w.io)).why, 'pairing');
  w = relayWorld({ offline: true });
  eq((await U.relayApp('https://h.example/app', w.io)).why, 'network');
  w = relayWorld(); w.io.commit = async () => w.res(500, 'rename failed');
  eq(await U.relayApp('https://h.example/app', w.io), { ok: false, why: 'refused', detail: 'commit: rename failed' });
  ['network', 'pairing', 'firmware', 'changed', 'refused'].forEach((why) => assert(!/failed$/.test(U.appWhyText(why)), why));

  console.log('ok (relay)');
})().catch((e) => { console.error(e); process.exit(1); });

// ---- the progress screen's steps ----
const PLAN = [{ board: 'main', version: 'd18f9b2', size: 871968 }, { board: 'sidekick', version: 'd18f9b2', size: 1005232 }];
const pst = (o) => Object.assign({ state: 'idle', board: 'main', step: 0, plan: PLAN, got: 0, size: 871968, why: '', at: '' }, o);
const marks = (status, reachable) => U.stepRows(status, reachable !== false).map((r) => r.at[0]).join('');
eq(U.stepRows(pst({ state: 'verifying' }), true).map((r) => r.text), [
  'Download the main board’s image (852 KB)', 'Verify it', 'Send it to the main board', 'Restart the main board into d18f9b2',
  'Download the sidekick’s image (982 KB)', 'Verify it', 'Restart the sidekick into d18f9b2',
]);
// d done, n now, x next (the first letter of 'next' is n too, so spell them out below).
const ats = (status, reachable) => U.stepRows(status, reachable !== false).map((r) => r.at);
const count = (a, v) => a.filter((x) => x === v).length;
let a = ats(pst({ state: 'downloading' }));
eq([a[0], count(a, 'next')], ['now', 6]);
a = ats(pst({ state: 'transferring', got: 435984 }));
eq(a, ['done', 'done', 'now', 'next', 'next', 'next', 'next']);
// The percentage is apart from the text, on the step in hand only: the sheet draws it at the end of
// the row's first line, where a long step's wrap cannot leave it on a line of its own.
eq(U.stepRows(pst({ state: 'transferring', got: 435984 }), true)[2], { text: 'Send it to the main board', pct: '50%', at: 'now' });
eq(U.stepRows(pst({ state: 'downloading', got: 87196 }), true)[0], { text: 'Download the main board’s image (852 KB)', pct: '9%', at: 'now' });
eq(U.stepRows(pst({ state: 'transferring', got: 435984 }), true).map((r) => r.pct), ['', '', '50%', '', '', '', '']);
eq(U.stepRows(pst({ state: 'verifying' }), true).map((r) => r.pct).join(''), '');
a = ats(pst({ state: 'downloading', step: 1, board: 'sidekick', size: 1005232 }));
eq(a, ['done', 'done', 'done', 'done', 'now', 'next', 'next']);
a = ats(pst({ state: 'restarting', step: 1, board: 'sidekick' }));
eq(a, ['done', 'done', 'done', 'done', 'done', 'done', 'now']);
// The sidekick stopped answering: its own restart, the plan's last step.
a = ats(pst({ state: 'verifying', step: 1 }), false);
assert.strictEqual(a[6], 'now');
// A failure stays on the step it happened in.
a = ats(pst({ state: 'failed', at: 'transferring', why: 'link' }));
eq(a, ['done', 'done', 'now', 'next', 'next', 'next', 'next']);
// A main-board-only plan that finished: everything done.
a = ats(pst({ state: 'done', plan: [PLAN[0]] }));
eq(a, ['done', 'done', 'done', 'done']);
eq(U.stepRows(null, true), []);
eq(U.stepRows(pst({ plan: [{ board: 'main', version: REL, size: 871968 }] }), true)[3].text, 'Restart the main board into 0.1.0');

assert.strictEqual(U.busy(null), false);
['downloading', 'verifying', 'transferring', 'restarting'].forEach((state) => assert.strictEqual(U.busy({ state }), true, state));
['idle', 'done', 'failed'].forEach((state) => assert.strictEqual(U.busy({ state }), false, state));

// ---- a start request that did not get through ----
// The plan's steps, as plan() gives them: each board and the version it goes to.
const STEPS = PLAN.map((b) => ({ board: b.board, to: b.version }));
// The sidekick is not updating, or did not answer the question either: send it once more. An
// earlier try's end names the plan a Retry sends, so it is not this request having arrived.
assert.strictEqual(U.startCheck(null, STEPS), 'resend');
['idle', 'done', 'failed'].forEach((state) => assert.strictEqual(U.startCheck(pst({ state }), STEPS), 'resend', state));
// It is carrying this plan out: the request arrived, and a second would be refused as busy.
['downloading', 'verifying', 'transferring', 'restarting'].forEach((state) =>
  assert.strictEqual(U.startCheck(pst({ state, step: 1, board: 'sidekick' }), STEPS), 'started', state));
// It is carrying out another plan: fewer boards, another version, another order, or none it names.
assert.strictEqual(U.startCheck(pst({ state: 'downloading', plan: [PLAN[0]] }), STEPS), 'busy');
assert.strictEqual(U.startCheck(pst({ state: 'downloading', plan: [PLAN[0], Object.assign({}, PLAN[1], { version: 'b1322+d18f9b2' })] }), STEPS), 'busy');
assert.strictEqual(U.startCheck(pst({ state: 'downloading', plan: [PLAN[1], PLAN[0]] }), STEPS), 'busy');
assert.strictEqual(U.startCheck({ state: 'transferring' }, STEPS), 'busy');

// ---- progress ----
const st = (o) => Object.assign({ state: 'idle', board: 'sidekick', version: 'd18f9b2', got: 0, size: 1005232, why: '', detail: 0, running: '7e61488', online: true }, o);
assert.strictEqual(U.progressText(null, true), 'Starting…');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 0 }), true), 'Downloading… 0%');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 432249 }), true), 'Downloading… 42%');
assert.strictEqual(U.progressText(st({ state: 'downloading', got: 1005231 }), true), 'Downloading… 99%', 'never 100% before it is all there');
assert.strictEqual(U.progressText(st({ state: 'verifying', got: 1005232 }), true), 'Verifying…');
assert.strictEqual(U.progressText(st({ state: 'restarting' }), true), 'Restarting the sidekick…');
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

// A channel's version is said by its label, and still matched whole.
eq(U.outcome(last('ok', { running: V, last: { board: 'sidekick', version: V, from: 'b1321+7e61488', result: 'ok' } }), 'sidekick', V),
  { ok: true, text: 'Updated to b1322' });
eq(U.outcome(st({ state: 'failed', why: 'hash', running: REL }), 'sidekick', V), { ok: false, text: 'Verification failed — still on 0.1.0' });
assert.strictEqual(U.outcome(last('ok'), 'sidekick', V), null, 'the bare commit is another version');

// A main board's update: the sidekick stays up, so the state says how it ended.
const mst = (o) => st(Object.assign({ board: 'main', size: 871968, host: '7e61488', host_linked: true }, o));
['downloading', 'verifying', 'transferring', 'restarting'].forEach(function (state) {
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
assert(codes.size >= 20, 'found the sidekick’s codes: ' + [...codes].join(' '));
for (const c of codes) {
  assert(!/^Update failed/.test(U.whyText(c)), 'no words for the sidekick’s "' + c + '"');
}
assert.strictEqual(U.whyText('nonsense'), 'Update failed (nonsense)');
assert.strictEqual(U.whyText(''), 'Update failed');

console.log('ok');
