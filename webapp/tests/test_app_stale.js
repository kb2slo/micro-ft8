#!/usr/bin/env node
// A page that is no longer the app the sidekick serves offers Reload (docs/architecture/webapp.md,
// Settings › Software update). The rule itself is FwUpdates.appStale() (test_updates.js); this
// runs app.html's own updateNetInfo() and noteServedApp(), and holds that every config reply
// reaches them: one the main board answered and one it did not take different paths through the
// page, and a check wired into only one of them never ran on a working station.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');
const updates = fs.readFileSync(path.join(__dirname, '..', 'app', 'updates.js'), 'utf8');

function extractFunction(text, name) {
  const start = text.indexOf('function ' + name + '(');
  assert(start !== -1, 'function not found in app.html: ' + name);
  let depth = 0, i = text.indexOf('{', start);
  let inStr = null, inLine = false, inBlock = false;
  for (; i < text.length; i++) {
    const c = text[i], prev = text[i - 1];
    if (inLine) { if (c === '\n') inLine = false; continue; }
    if (inBlock) { if (c === '/' && prev === '*') inBlock = false; continue; }
    if (inStr) { if (c === inStr && prev !== '\\') inStr = null; continue; }
    if (c === '/' && text[i + 1] === '/') { inLine = true; continue; }
    if (c === '/' && text[i + 1] === '*') { inBlock = true; continue; }
    if (c === '"' || c === "'") { inStr = c; continue; }
    if (c === '{') depth++;
    if (c === '}' && --depth === 0) { i++; break; }
  }
  return text.slice(start, i);
}

const V = 'b1322+d18f9b2';
const els = {};
let stored = {}, reloads = 0;
const box = {
  APP_BUILD: V, appIsStale: false, appServed: '', fwSheetMode: '', fwReloadOnClose: false,
  APP_RELOADED_FOR: 'appReloadedFor',
  sessionStorage: { getItem: (k) => (k in stored ? stored[k] : null), setItem: (k, v) => { stored[k] = String(v); } },
  location: { reload: () => { reloads++; } },
  fwRunning: {}, fwLink: { hostLinked: false, sidekickProto: 0, mainProto: 0 },
  renderFwUpdates: () => {}, updateApInfo: () => {},
  document: { getElementById: (id) => els[id] || (els[id] = { textContent: '', hidden: id === 'appSheet', focus() { els.focused = id; } }) },
};
vm.createContext(box);
vm.runInContext(updates, box);
['appReloadedFor', 'reloadForApp', 'noteServedApp', 'syncAppSheet', 'fwCloseReloads', 'updateNetInfo'].forEach((name) =>
  vm.runInContext(extractFunction(src, name), box));
const reply = (o) => vm.runInContext('updateNetInfo(' + JSON.stringify(Object.assign({ sk_wifi_mode: 'station' }, o)) + ')', box);
const shown = () => [!els.appStale.hidden, box.appIsStale, !els.appPushedRow.hidden, els.appPushed.textContent];

// The app the sidekick serves is this page: one version says both, and nothing is offered.
reply({ sk_fw_version: V, sk_app: V });
assert.deepStrictEqual(shown(), [false, false, false, '—']);
assert.strictEqual(els.netVersion.textContent, 'b1322 (d18f9b2)', 'the Sidekick and app line');
// The sidekick was updated under the page: Reload is offered, from the reply alone.
reply({ sk_fw_version: 'b1323+e6e4d88', sk_app: 'b1323+e6e4d88' });
assert.deepStrictEqual(shown(), [true, true, false, '—']);
// And cleared by a reply that names this page's app again (a roll back to it).
reply({ sk_fw_version: V, sk_app: V });
assert.deepStrictEqual(shown(), [false, false, false, '—']);
// A bench push: the page is not it, and Settings names it on its own line.
reply({ sk_fw_version: V, sk_app: 'd18f9b2-dirty 2026-11-15 04:37:12' });
assert.deepStrictEqual(shown(), [true, true, true, 'd18f9b2-dirty 2026-11-15 04:37:12']);
// The page that was loaded from that push is not stale, and still says it is a pushed app.
box.APP_BUILD = 'd18f9b2-dirty 2026-11-15 04:37:12';
reply({ sk_fw_version: V, sk_app: 'd18f9b2-dirty 2026-11-15 04:37:12' });
assert.deepStrictEqual(shown(), [false, false, true, 'd18f9b2-dirty 2026-11-15 04:37:12']);
// The next push, a second later, is another app.
reply({ sk_fw_version: V, sk_app: 'd18f9b2-dirty 2026-11-15 04:37:13' });
assert.deepStrictEqual(shown(), [true, true, true, 'd18f9b2-dirty 2026-11-15 04:37:13']);
// A sidekick that does not say (firmware from before the app shipped in its image, the simulator,
// the page's own cache, which keeps no sk_* facts): nothing to compare.
box.APP_BUILD = V;
reply({ sk_fw_version: 'e0c973b' });
assert.deepStrictEqual(shown(), [false, false, false, '—']);
reply({});
assert.deepStrictEqual(shown(), [false, false, false, '—']);

// ---- the sheet over every tab ----
const sheet = () => !els.appSheet.hidden;
box.APP_BUILD = V;
reply({ sk_fw_version: V, sk_app: V });
assert.strictEqual(sheet(), false, 'nothing to say while the page is the served app');
// The app changed under the page: the sheet comes up by itself, with Reload in focus. It has the
// one button: there is nothing to decide.
reply({ sk_fw_version: 'b1323+e6e4d88', sk_app: 'b1323+e6e4d88' });
assert.deepStrictEqual([sheet(), els.focused], [true, 'appSheetReload']);
assert(!/Not now/.test(src.slice(src.indexOf('<div id=appSheet'), src.indexOf('<div id=resetWait'))), 'no way to put it off');
// It goes by itself when the sidekick serves this page's app again.
reply({ sk_fw_version: V, sk_app: V });
assert.strictEqual(sheet(), false);

// The guard. Reload notes which app it is for; the page that then loads is that app, normally.
reply({ sk_fw_version: 'b1323+e6e4d88', sk_app: 'b1323+e6e4d88' });
vm.runInContext('reloadForApp()', box);
assert.deepStrictEqual([reloads, stored.appReloadedFor], [1, 'b1323+e6e4d88']);
// A broken setup: the fresh page is still the old one (a cached copy handed back, or two sidekicks
// on one name). The sheet does not come back for that app, so the operator is not shut out;
// Settings goes on saying it.
els.appSheet.hidden = true;
reply({ sk_fw_version: 'b1323+e6e4d88', sk_app: 'b1323+e6e4d88' });
assert.deepStrictEqual([sheet(), !els.appStale.hidden, box.appIsStale], [false, true, true]);
// Another app (the next update, or the next bench push) is another matter.
reply({ sk_fw_version: 'b1324+0c5ccec', sk_app: 'b1324+0c5ccec' });
assert.strictEqual(sheet(), true);
// A browser that refuses storage has no guard, and still has the sheet.
const real = box.sessionStorage;
box.sessionStorage = { getItem: () => { throw new Error('denied'); }, setItem: () => { throw new Error('denied'); } };
els.appSheet.hidden = true;
reply({ sk_fw_version: 'b1323+e6e4d88', sk_app: 'b1323+e6e4d88' });
assert.strictEqual(sheet(), true);
vm.runInContext('reloadForApp()', box);
assert.strictEqual(reloads, 2, 'and Reload still reloads');
box.sessionStorage = real;
stored = {};
reply({ sk_fw_version: V, sk_app: V });

// Never over the update's own sheet: that one's Close reloads, on the phone that ran the update
// and on one that only watched it and is now older than the app.
box.fwSheetMode = 'result';
reply({ sk_fw_version: 'b1325+abc1234', sk_app: 'b1325+abc1234' });
assert.deepStrictEqual([sheet(), box.fwCloseReloads()], [false, true]);
box.fwReloadOnClose = false;
box.appIsStale = false;
assert.strictEqual(box.fwCloseReloads(), false, 'a watcher whose app did not change is not reloaded');
box.fwReloadOnClose = true;
assert.strictEqual(box.fwCloseReloads(), true);
box.fwReloadOnClose = false;
// When the update's sheet goes without a reload, the app's own shows if it is still wanted.
box.appIsStale = true;
box.fwSheetMode = '';
vm.runInContext('syncAppSheet()', box);
assert.strictEqual(sheet(), true);
assert(/\bsyncAppSheet\(\);/.test(extractFunction(src, 'fwShow')), 'fwShow() lets the app sheet follow the update sheet');
assert(/fwCloseReloads\(\)/.test(src.slice(src.indexOf("getElementById('fwCancel').onclick"))), 'Close asks fwCloseReloads()');

// Every config reply reaches it. A reply the main board answered is applied by applyOperateLive();
// one it did not answer ({"ok":false}, which still carries the sidekick's facts) by
// applySidekickFacts(). Both call updateNetInfo(), which is where the check lives.
assert(/\bnoteServedApp\(cfg\);/.test(extractFunction(src, 'updateNetInfo')), 'updateNetInfo() checks the served app');
['applyOperateLive', 'applySidekickFacts'].forEach((name) =>
  assert(/\bupdateNetInfo\(cfg\);/.test(extractFunction(src, name)), name + '() reaches updateNetInfo()'));
assert.strictEqual((src.match(/\bnoteServedApp\(cfg\)/g) || []).length, 2, 'defined once, called from the one place every reply reaches');

console.log('ok (app stale)');
