#!/usr/bin/env node
// Settings' menu (docs/architecture/webapp.md, Settings): runs app.html's own settingsMenuModel(),
// which says what each row reads under its name and when it turns amber. A row that wants
// attention says so in words and a glyph as well as the tint, and the Software update row's
// order is: an update running, a firmware update, a newer app, then what the check found.
// Also holds the markup to the model: every page has a row, every row a page, and a Back.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');
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

const ctx = {};
vm.createContext(ctx);
vm.runInContext(extractFunction(src, 'settingsMenuModel'), ctx);
const model = (over) => ctx.settingsMenuModel(Object.assign({
  call: 'KB2SLO', grid: 'EM10cg', cq: 'CQ POTA', radio: 'QMX', bands: ['40m', '20m'], rxFt4: false, amp: true, ampPtt: 0, ampBad: false,
  txDelay: '50', netMode: 'station', netName: 'home', netIp: '192.168.1.20', paired: true,
  fwBoards: [], fwChecked: true, fwNoNet: false, fwBusy: false, appStale: false, units: 'km', theme: 'system',
}, over || {}));
const row = (r) => [r.text, r.tone, r.glyph];

// A station with nothing wrong: every row quiet.
let m = model();
assert.deepStrictEqual(row(m.station), ['KB2SLO · EM10 · CQ POTA', '', '']);
assert.deepStrictEqual(row(m.radio), ['QMX · 40m 20m', '', '']);
assert.deepStrictEqual(row(m.amp), ['Keying not checked · 50 ms delay', '', '']);
assert.deepStrictEqual(row(m.network), ['WiFi · home · 192.168.1.20', '', '']);
assert.deepStrictEqual(row(m.pairing), ['This browser is paired', '', '✓']);
assert.deepStrictEqual(row(m.update), ['No updates', '', '✓']);
assert.deepStrictEqual(row(m.device), ['Kilometers · Theme matches phone', '', '']);
assert.strictEqual(m.diag, undefined, 'Diagnostics keeps its fixed line');

// Station and radio.
assert.deepStrictEqual(row(model({ call: '' }).station), ['No call set', 'amber', '!']);
assert.strictEqual(model({ grid: '' }).station.text, 'KB2SLO · CQ POTA');
assert.strictEqual(model({ rxFt4: true }).radio.text, 'QMX · 40m 20m · FT4 too');
assert.strictEqual(model({ bands: [] }).radio.text, 'QMX · no bands');
assert.strictEqual(model({ radio: '' }).radio.text, '40m 20m');

// Amplifier: Off says so and nothing else, whatever is saved under it; On, amber only when a
// keying is chosen and the check found a band wrong.
assert.deepStrictEqual(row(model({ amp: false, ampPtt: 1, ampBad: true }).amp), ['Off', '', '']);
assert.deepStrictEqual(row(model({ ampPtt: 1 }).amp), ['Keyed by TX PTT grounded · 50 ms delay', '', '']);
assert.strictEqual(model({ ampPtt: 2, txDelay: '0' }).amp.text, 'Keyed by TX PTT +5V · 0 ms delay');
assert.strictEqual(model({ ampPtt: 2, txDelay: '' }).amp.text, 'Keyed by TX PTT +5V');
assert.deepStrictEqual(row(model({ ampPtt: 1, ampBad: true }).amp), ['The radio will not key the amp as set', 'amber', '!']);
assert.strictEqual(model({ ampPtt: 0, ampBad: true }).amp.tone, '', 'not checked is never a warning');

// Network: the mode's word only when the sidekick has said which.
assert.strictEqual(model({ netMode: 'ap', netName: 'micro-ft8.local', netIp: '192.168.4.1' }).network.text,
  'Own network · micro-ft8.local · 192.168.4.1');
assert.strictEqual(model({ netMode: undefined, netName: '', netIp: '' }).network.text, 'WiFi and the sidekick\'s own network');

// Pairing: the radio bar's words.
assert.deepStrictEqual(row(model({ paired: false }).pairing), ['Not paired · press the sidekick\'s button', 'amber', '!']);

// Software update, highest first.
assert.deepStrictEqual(row(model({ fwBusy: true, fwBoards: ['Main board'], appStale: true }).update), ['Updating firmware', 'amber', '…']);
assert.deepStrictEqual(row(model({ fwBoards: ['Main board', 'Sidekick'], appStale: true }).update),
  ['Update available · Main board, Sidekick', 'amber', '!']);
assert.deepStrictEqual(row(model({ appStale: true }).update), ['A newer app is available · reload', 'amber', '!']);
assert.deepStrictEqual(row(model({ fwChecked: false, fwNoNet: true }).update), ['Cannot be checked from here', '', '']);
assert.deepStrictEqual(row(model({ fwChecked: false }).update), ['Not checked yet', '', '']);

// This device.
assert.strictEqual(model({ units: 'mi', theme: 'dark' }).device.text, 'Miles · Dark');
assert.strictEqual(model({ theme: 'light' }).device.text, 'Kilometers · Light');

// The markup: a row for every page and a page for every row, each page with a Back and a title,
// and no id twice (the pages were one long form; a control moved is a control that could be left behind).
const section = src.slice(src.indexOf('<section id=view-settings>'), src.indexOf("<!-- Operate's pinned bottom"));
const rows = [...section.matchAll(/class=navrow data-page=(\w+)/g)].map((x) => x[1]);
const pages = [...section.matchAll(/class="setpage sub" data-page=(\w+)/g)].map((x) => x[1]);
assert.deepStrictEqual(rows, ['station', 'radio', 'amp', 'network', 'pairing', 'update', 'device', 'diag']);
assert.deepStrictEqual(pages, rows);
pages.forEach((p) => assert(section.includes('id=setp-' + p + ' '), 'page id for ' + p));
assert.strictEqual((section.match(/class=setback/g) || []).length, pages.length);
assert.strictEqual((section.match(/<h2 tabindex=-1>/g) || []).length, pages.length);
Object.keys(m).forEach((k) => assert(rows.includes(k), 'the model names a row the menu lacks: ' + k));
const ids = [...src.matchAll(/\sid=["']?([A-Za-z_][\w-]*)/g)].map((x) => x[1]);
const twice = ids.filter((id, i) => ids.indexOf(id) !== i);
assert.deepStrictEqual(twice, [], 'ids used twice in app.html');

console.log('test_settings_menu: ok');
