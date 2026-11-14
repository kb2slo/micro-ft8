#!/usr/bin/env node
// The CQ's modifier on Settings › Station (webapp.md, Settings › Station; autoseq.md, The CQ's
// modifier): runs app.html's own cqModOk(), cqModChoice() and cqSendsText(). The accepted and
// refused modifiers are the vectors host_mock/test_station.cpp holds station_cq_mod_ok() to, so
// the sheet refuses what the main board refuses.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');

function extractFunction(text, name) {
  const start = text.indexOf('function ' + name + '(');
  assert(start !== -1, 'function not found in app.html: ' + name);
  let depth = 0, i = text.indexOf('{', start), inStr = null, lc = false, bc = false;
  for (; i < text.length; i++) {
    const c = text[i], prev = text[i - 1];
    if (lc) { if (c === '\n') lc = false; continue; }
    if (bc) { if (c === '/' && prev === '*') bc = false; continue; }
    if (inStr) { if (c === inStr && prev !== '\\') inStr = null; continue; }
    if (c === '/' && text[i + 1] === '/') { lc = true; continue; }
    if (c === '/' && text[i + 1] === '*') { bc = true; continue; }
    if (c === '"' || c === "'") { inStr = c; continue; }
    if (c === '{') depth++;
    if (c === '}' && --depth === 0) { i++; break; }
  }
  return text.slice(start, i);
}

const presets = src.match(/var CQ_MOD_PRESETS = \[[^\]]*\];/);
assert(presets, 'CQ_MOD_PRESETS not found in app.html');
const sb = { lastCfg: {} };
vm.createContext(sb);
vm.runInContext(presets[0] + '\n' +
  ['cqModOk', 'cqModChoice', 'cqSendsText'].map((n) => extractFunction(src, n)).join('\n'), sb);

// The same vectors as host_mock/test_station.cpp.
for (const m of ['', 'DX', 'POTA', 'SOTA', 'QRP', 'HI', 'AK', 'A', 'TEST', '123', '000']) {
  assert.strictEqual(sb.cqModOk(m), true, 'accepted: "' + m + '"');
}
for (const m of ['POTAS', 'D X', 'DX1', '12', '1234', '1A', 'dx', 'CQ DX', '-', 'US-1']) {
  assert.strictEqual(sb.cqModOk(m), false, 'refused: "' + m + '"');
}

// Type shows a modifier offered by name as itself, any other as Other, and stays on Other while
// the operator has picked it and not yet typed one.
assert.strictEqual(sb.cqModChoice('', false), '');
assert.strictEqual(sb.cqModChoice('DX', false), 'DX');
assert.strictEqual(sb.cqModChoice('POTA', false), 'POTA');
assert.strictEqual(sb.cqModChoice('HI', false), '*');
assert.strictEqual(sb.cqModChoice('123', false), '*');
assert.strictEqual(sb.cqModChoice('DX', true), '*');
assert.strictEqual(sb.cqModChoice('', true), '*');

// Every option Type offers by name is one the page's own check accepts, and is in the markup.
for (const m of sb.CQ_MOD_PRESETS) {
  assert.strictEqual(sb.cqModOk(m), true, m);
  assert(src.includes('<option value=' + m + '>CQ ' + m + '</option>'), 'Type offers CQ ' + m);
}

// The Sends text is what autoseq's generate_cq_text_into() renders.
sb.lastCfg = { call: 'KB2SLO', grid: 'EM10cg', cq_mod: '' };
assert.strictEqual(sb.cqSendsText(), 'CQ KB2SLO EM10');
sb.lastCfg.cq_mod = 'DX';
assert.strictEqual(sb.cqSendsText(), 'CQ DX KB2SLO EM10');
sb.lastCfg.cq_mod = 'HI';
assert.strictEqual(sb.cqSendsText(), 'CQ HI KB2SLO EM10');

console.log('PASS: the CQ modifier check, Type\'s choice and the Sends text');
