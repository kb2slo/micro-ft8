#!/usr/bin/env node
// architecture/logging.md: n-fer expansion happens at export, not on the
// device -- one POTA file per park per day, one SOTA file per summit, each
// carrying only that reference's own fields (MY_SIG/MY_SIG_INFO or
// MY_SOTA_REF), since pota.app's real importer does not read the device's
// own MY_POTA_REF comma list (confirmed against its importer and PoLo's
// notes, RFC 0008 §3).
//
// Same approach as test_decode_batching.js: pull the exact function source
// out of app.html and run it under Node, rather than re-implementing the
// grouping/export logic a second time here.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const APP_HTML = path.join(__dirname, '..', 'app', 'app.html');
const src = fs.readFileSync(APP_HTML, 'utf8');

function extractFunction(text, name) {
  const marker = 'function ' + name + '(';
  const start = text.indexOf(marker);
  assert(start !== -1, 'function not found in app.html: ' + name);
  const braceStart = text.indexOf('{', start);
  assert(braceStart !== -1, 'no body found for: ' + name);
  let depth = 0;
  let inStr = null, inLineComment = false, inBlockComment = false;
  let i = braceStart;
  for (; i < text.length; i++) {
    const c = text[i];
    const prev = text[i - 1];
    if (inLineComment) { if (c === '\n') inLineComment = false; continue; }
    if (inBlockComment) { if (c === '/' && prev === '*') inBlockComment = false; continue; }
    if (inStr) { if (c === inStr && prev !== '\\') inStr = null; continue; }
    if (c === '/' && text[i + 1] === '/') { inLineComment = true; continue; }
    if (c === '/' && text[i + 1] === '*') { inBlockComment = true; continue; }
    if (c === '"' || c === "'") { inStr = c; continue; }
    if (c === '{') { depth++; continue; }
    if (c === '}') {
      depth--;
      if (depth === 0) { i++; break; }
    }
  }
  assert(depth === 0, 'unbalanced braces extracting: ' + name);
  return text.slice(start, i);
}

// BANDS is a multi-line `var NAME = [ ... ];` array literal (no nested
// strings containing brackets), so this just needs to balance [ ] / { }
// rather than also skipping comments -- extractFunction's fuller version is
// overkill for this one declaration.
function extractVarBlock(text, name) {
  const marker = 'var ' + name + ' =';
  const start = text.indexOf(marker);
  assert(start !== -1, 'var not found in app.html: ' + name);
  let i = text.indexOf('[', start);
  const braceStart = text.indexOf('{', start);
  if (braceStart !== -1 && (i === -1 || braceStart < i)) i = braceStart;
  assert(i !== -1, 'no [ or { found for: ' + name);
  let depth = 0;
  let j = i;
  for (; j < text.length; j++) {
    const c = text[j];
    if (c === '[' || c === '{') depth++;
    if (c === ']' || c === '}') {
      depth--;
      if (depth === 0) { j++; break; }
    }
  }
  while (text[j] !== ';') j++;
  j++;
  return text.slice(start, j);
}

const extracted = [
  extractVarBlock(src, 'BANDS'),
  extractFunction(src, 'adifField'),
  extractFunction(src, 'adifFieldIfSet'),
  extractFunction(src, 'adifBandTag'),
  extractFunction(src, 'buildAdifRecord'),
  extractFunction(src, 'dayActivationRefs'),
  extractFunction(src, 'groupRowsByReference'),
  extractFunction(src, 'exportFileBaseName'),
].join('\n\n');

function freshSandbox() {
  const sandbox = { console: console };
  vm.createContext(sandbox);
  vm.runInContext(extracted, sandbox, { filename: 'app.html (extracted)' });
  return sandbox;
}

// A QsoLogEntry as /api/log/entries actually serves it (host_link.c's
// file_entries_json_add()) -- field names match that JSON exactly.
function row(over) {
  return Object.assign({
    time: '19:04', band: '20m', call: 'W1ABC', rst_sent: 3, rst_rcvd: -11,
    grid: 'FN42', freq: '14.074', my_grid: 'FN30', comment: '',
    mode: 'FT8', station_callsign: 'KB2SLO', my_pota_ref: '', my_sota_ref: '',
  }, over || {});
}

// vm.createContext() gives the sandbox its own realm, so an object it
// returns has a different Object/Array prototype than this file's own --
// assert.deepStrictEqual treats that as unequal even when every property
// matches. Round-tripping through JSON strips the cross-realm prototype
// without touching the plain data these checks actually care about.
function plain(x) { return JSON.parse(JSON.stringify(x)); }

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log('PASS: ' + name);
  } catch (e) {
    failures++;
    console.log('FAIL: ' + name);
    console.log('  ' + (e && e.message ? e.message : e));
  }
}

// ---- dayActivationRefs / groupRowsByReference ----------------------------

check('a no-activation day has no references and one empty-set bucket list', () => {
  const sb = freshSandbox();
  const rows = [row(), row({ call: 'K9ZZZ' })];
  const refs = sb.dayActivationRefs(rows);
  assert.deepStrictEqual(plain(refs), { pota: [], sota: [] });
  assert.deepStrictEqual(plain(sb.groupRowsByReference(rows)), []);
});

check('a single-reference day groups every row into one POTA bucket', () => {
  const sb = freshSandbox();
  const rows = [row({ my_pota_ref: 'US-1234' }), row({ call: 'K9ZZZ', my_pota_ref: 'US-1234' })];
  const refs = sb.dayActivationRefs(rows);
  assert.deepStrictEqual(plain(refs), { pota: ['US-1234'], sota: [] });
  const buckets = sb.groupRowsByReference(rows);
  assert.strictEqual(buckets.length, 1);
  assert.strictEqual(buckets[0].program, 'POTA');
  assert.strictEqual(buckets[0].ref, 'US-1234');
  assert.strictEqual(buckets[0].rows.length, 2);
});

check('a 2-fer day (two parks worked concurrently) makes two POTA buckets, sharing rows', () => {
  const sb = freshSandbox();
  const rows = [
    row({ my_pota_ref: 'US-1234,US-5678' }),  // worked during both activations
    row({ call: 'K9ZZZ', my_pota_ref: 'US-1234' }),
    row({ call: 'N0CALL', my_pota_ref: 'US-5678' }),
  ];
  const refs = sb.dayActivationRefs(rows);
  assert.deepStrictEqual(plain(refs), { pota: ['US-1234', 'US-5678'], sota: [] });
  const buckets = sb.groupRowsByReference(rows);
  assert.strictEqual(buckets.length, 2);
  const us1234 = buckets.find((b) => b.ref === 'US-1234');
  const us5678 = buckets.find((b) => b.ref === 'US-5678');
  assert.strictEqual(us1234.rows.length, 2, 'the 2-fer QSO plus the US-1234-only one');
  assert.strictEqual(us5678.rows.length, 2, 'the 2-fer QSO plus the US-5678-only one');
});

check('a mixed POTA+SOTA day counts and groups each program independently', () => {
  const sb = freshSandbox();
  const rows = [
    row({ my_pota_ref: 'US-1234' }),
    row({ call: 'K9ZZZ', my_sota_ref: 'W7O/CC-001' }),
  ];
  const refs = sb.dayActivationRefs(rows);
  assert.deepStrictEqual(plain(refs), { pota: ['US-1234'], sota: ['W7O/CC-001'] });
  const buckets = sb.groupRowsByReference(rows);
  assert.strictEqual(buckets.length, 2);
  assert.strictEqual(buckets[0].program, 'POTA', 'POTA sorts before SOTA');
  assert.strictEqual(buckets[1].program, 'SOTA');
});

// ---- buildAdifRecord: the n-fer file only carries its own reference ------

check('a POTA bucket writes MY_SIG/MY_SIG_INFO, not the raw MY_POTA_REF list', () => {
  const sb = freshSandbox();
  const q = row({ my_pota_ref: 'US-1234,US-5678' });
  const out = sb.buildAdifRecord(q, '20260929', { program: 'POTA', ref: 'US-1234' });
  assert(out.includes('<my_sig:4>POTA '), out);
  assert(out.includes('<my_sig_info:7>US-1234 '), out);
  assert(!out.includes('my_pota_ref'), 'the comma list must not leak into the per-park file');
});

check('a SOTA bucket writes MY_SOTA_REF scoped to its one summit', () => {
  const sb = freshSandbox();
  const q = row({ my_sota_ref: 'W7O/CC-001' });
  const out = sb.buildAdifRecord(q, '20260929', { program: 'SOTA', ref: 'W7O/CC-001' });
  assert(out.includes('<my_sota_ref:10>W7O/CC-001 '), out);
  assert(!out.includes('my_sig'), 'SOTA files use my_sota_ref, not my_sig/my_sig_info');
});

check('without a sig argument, the raw comma lists pass through unchanged (the archive path)', () => {
  const sb = freshSandbox();
  const q = row({ my_pota_ref: 'US-1234,US-5678', my_sota_ref: 'W7O/CC-001' });
  const out = sb.buildAdifRecord(q, '20260929');
  assert(out.includes('<my_pota_ref:15>US-1234,US-5678 '), out);
  assert(out.includes('<my_sota_ref:10>W7O/CC-001 '), out);
  assert(!out.includes('my_sig'), out);
});

check('a known band gets a unit and upper case; an unrecognized one is omitted', () => {
  const sb = freshSandbox();
  const known = sb.buildAdifRecord(row({ band: '20m' }), '20260929');
  assert(known.includes('<band:3>20M '), known);
  const unknown = sb.buildAdifRecord(row({ band: '14.074' }), '20260929');
  assert(!unknown.includes('<band:'), 'a freq-string fallback is not a valid ADIF band');
  const none = sb.buildAdifRecord(row({ band: '?' }), '20260929');
  assert(!none.includes('<band:'), 'the "no band known" sentinel is not a valid ADIF band');
});

// ---- exportFileBaseName ---------------------------------------------------

check('a POTA filename is CALL@PARK-YYYYMMDD (architecture/logging.md)', () => {
  const sb = freshSandbox();
  const name = sb.exportFileBaseName('KB2SLO', { program: 'POTA', ref: 'US-1234' }, '20260929');
  assert.strictEqual(name, 'KB2SLO@US-1234-20260929');
});

check('a SOTA filename replaces the reference\'s own "/" so it stays one path component', () => {
  const sb = freshSandbox();
  const name = sb.exportFileBaseName('KB2SLO', { program: 'SOTA', ref: 'W7O/CC-001' }, '20260929');
  assert.strictEqual(name, 'KB2SLO@W7O-CC-001-20260929');
});

if (failures) {
  console.log(failures + ' FAIL(s)');
  process.exit(1);
}
console.log('All n-fer export checks passed');
