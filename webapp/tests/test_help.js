#!/usr/bin/env node
// Settings › Help & about (docs/architecture/webapp.md): runs app.html's own helpEmail(),
// radioReportText(), installedText() and reflowLicence(). The email carries the versions under the
// bug-report form's own questions; and licences.json, which tools/licences.py writes, holds a text for
// every item.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const root = path.join(__dirname, '..', '..');
const src = fs.readFileSync(path.join(root, 'webapp', 'app', 'app.html'), 'utf8');
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
const extractVar = (name) => {
  const m = src.match(new RegExp('var ' + name + ' = [^;]+;'));
  assert(m, 'var not found: ' + name);
  return m[0];
};

const box = { FwUpdates: { versionText: (v) => (v ? 'v(' + v + ')' : '') } };
vm.createContext(box);
vm.runInContext("var APP_NAME = 'micro-FT8';\n" + extractVar('HELP_EMAIL') + '\n' + extractVar('SHARE') + '\n' +
  ['versionLines', 'helpEmail', 'radioReportText', 'installedText', 'reflowLicence', 'shareMethod'].map((n) => extractFunction(src, n)).join('\n'), box);
const { versionLines, helpEmail, radioReportText, installedText, reflowLicence, shareMethod } = box;

let failed = 0;
function check(name, fn) {
  try { fn(); console.log('PASS  ' + name); } catch (e) { failed++; console.log('FAIL  ' + name + '\n      ' + e.message); }
}

const versions = [['Sidekick and app', 'b1464 (33631f7)'], ['Main board firmware', 'b1464 (33631f7)'], ['App, pushed from the bench', '']];

check('the email carries the same versions and the radio', () => {
  const m = helpEmail(versions, 'HL2 gateware 74.2');
  assert(m.startsWith('mailto:micro-ft8@kalikstein.com?'), m);
  const p = new URLSearchParams(m.split('?')[1]);
  assert.strictEqual(p.get('subject'), 'micro-FT8: ');
  assert(p.get('body').includes('Installed versions:\nSidekick and app: b1464 (33631f7)\nMain board firmware: b1464 (33631f7)\n'), p.get('body'));
  assert(p.get('body').includes('Radio and its firmware: HL2 gateware 74.2'), p.get('body'));
  const form = fs.readFileSync(path.join(root, '.github', 'ISSUE_TEMPLATE', 'bug_report.yml'), 'utf8');
  ['What happened', 'When, and how often', 'Installed versions', 'Radio and its firmware'].forEach((q) =>
    assert(form.includes('label: ' + q), 'the bug-report form no longer asks: ' + q));
});

check('the versions as copied: a line each, the ones not known left out', () => {
  assert.strictEqual(versionLines(versions.concat([['Radio and its firmware', 'QMX 1_04_010']])),
    'Sidekick and app: b1464 (33631f7)\nMain board firmware: b1464 (33631f7)\nRadio and its firmware: QMX 1_04_010');
});

check('a version not known is left out of the email', () => {
  const p = new URLSearchParams(helpEmail([['Sidekick and app', '']], '').split('?')[1]);
  assert(p.get('body').includes('Installed versions:\n(not known)'), p.get('body'));
});

check('the radio as a report names it', () => {
  assert.strictEqual(radioReportText('QMX', { radio: 1, radio_fw: '1_04_010QMX' }), 'QMX 1_04_010QMX');
  assert.strictEqual(radioReportText('HL2', { radio: 6, hl2_gw: '74.2' }), 'HL2 gateware 74.2');
  assert.strictEqual(radioReportText('QMX', { radio: 1 }), 'QMX');
});

check('Software update\'s one Installed line: the station\'s version, or each board\'s while they differ', () => {
  assert.strictEqual(installedText({ sk_fw_version: 'a', host_fw_version: 'a' }), 'v(a)');
  assert.strictEqual(installedText({ sk_fw_version: 'a', host_fw_version: 'b' }), 'Sidekick v(a) · Main board v(b)');
  assert.strictEqual(installedText({ sk_fw_version: 'a' }), 'v(a)');
  assert.strictEqual(installedText({}), '—');
});

check('a licence text is rewrapped, its paragraphs and list items kept', () => {
  assert.strictEqual(reflowLicence('Permission is\ngranted.\n\n1. One\nline.\n2. Two\n   indented'),
    'Permission is granted.\n\n1. One line.\n2. Two\n   indented');
});

check('Share: the phone\'s share sheet only where the page may open it, else a copy', () => {
  const share = { share: () => Promise.resolve() };
  assert.strictEqual(shareMethod(share, true), 'sheet');
  assert.strictEqual(shareMethod(share, false), 'copy', 'plain HTTP, as the sidekick serves the app');
  assert.strictEqual(shareMethod({}, true), 'copy', 'a browser with no share sheet');
  assert.strictEqual(shareMethod(Object.assign({ canShare: () => false }, share), true), 'copy');
  assert.strictEqual(box.SHARE.url, 'https://micro-ft8.kalikstein.com/', 'the site, never the station');
});

check('licences.json: every item has its text, and Mini-FT8 comes first', () => {
  const d = JSON.parse(fs.readFileSync(path.join(root, 'webapp', 'app', 'licences.json'), 'utf8'));
  assert.strictEqual(d.items[0].name, 'Mini-FT8');
  d.items.forEach((it) => {
    assert(d.texts[it.text] && d.texts[it.text].length > 200, 'no text for ' + it.name);
    assert(it.where.length, 'nowhere for ' + it.name);
  });
  assert.deepStrictEqual(Object.keys(d.texts).filter((k) => !d.items.some((it) => it.text === k)), [], 'a text no item uses');
});

if (failed) { console.log(failed + ' failed'); process.exit(1); }
console.log('all passed');
