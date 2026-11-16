#!/usr/bin/env node
// Text fields (docs/architecture/webapp.md, Fields): every text input in webapp/app says what a tap does
// with data-select ("all", or "off:<one word why>"), and none is type=number. A new field fails here until
// it makes that choice; fields.js does what "all" asks.
'use strict';
const fs = require('fs');
const path = require('path');

const app = path.join(__dirname, '..', 'app');
const NOT_TEXT = /\btype=["']?(checkbox|radio|hidden|file|range|button|submit|color)\b/i;
let failed = false;
let checked = 0;
for (const name of fs.readdirSync(app).filter((f) => /\.(html|js)$/.test(f)).sort()) {
  const src = fs.readFileSync(path.join(app, name), 'utf8');
  for (const m of src.matchAll(/<input\b[^>]*>/gi)) {
    const tag = m[0];
    const line = src.slice(0, m.index).split('\n').length;
    if (/\btype=["']?number\b/i.test(tag)) {
      console.error(`FAIL ${name}:${line}: type=number; use inputmode for the keypad (Fields): ${tag}`);
      failed = true;
    }
    if (NOT_TEXT.test(tag)) continue;
    checked++;
    const sel = tag.match(/\bdata-select=["']?([^"'\s>]+)/i);
    if (!sel || !/^(all|off:[a-z]+)$/.test(sel[1])) {
      console.error(`FAIL ${name}:${line}: a text input needs data-select=all or data-select=off:<why> (Fields): ${tag}`);
      failed = true;
    }
  }
  // An input built in script passes only when it is given a non-text type within a few lines.
  for (const m of src.matchAll(/createElement\(\s*['"]input['"]\s*\)/g)) {
    const after = src.slice(m.index, m.index + 200);
    if (/\.type\s*=\s*['"](checkbox|radio|hidden|file|range|button|submit|color)['"]/.test(after)) continue;
    const line = src.slice(0, m.index).split('\n').length;
    console.error(`FAIL ${name}:${line}: a text input built in script is not checked here; write it as markup (Fields)`);
    failed = true;
  }
}
if (!checked) { console.error('FAIL: no text inputs found; is the path right?'); failed = true; }
if (failed) process.exit(1);
console.log(`fields: ok (${checked} text inputs)`);
