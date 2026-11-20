#!/usr/bin/env node
// Settings › Station's Offset src (docs/architecture/autoseq.md, TX offset): each choice sends the value the
// firmware gives that meaning (components/autoseq/include/tx_offset.h). The labels were once paired with
// each other's values, so RX held the Offset Hz cursor and Fixed followed the other station.
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const root = path.join(__dirname, '..', '..');
const html = fs.readFileSync(path.join(root, 'webapp', 'app', 'app.html'), 'utf8');
const hdr = fs.readFileSync(path.join(root, 'components', 'autoseq', 'include', 'tx_offset.h'), 'utf8');

const constant = (name) => {
  const m = new RegExp('constexpr int ' + name + ' = (\\d+);').exec(hdr);
  assert(m, 'tx_offset.h has no ' + name);
  return m[1];
};
const select = /<select id=offset_src>([\s\S]*?)<\/select>/.exec(html);
assert(select, 'app.html has no select#offset_src');
const options = {};
for (const m of select[1].matchAll(/<option value=(\d+)>([^<]+)<\/option>/g)) options[m[2].trim()] = m[1];

assert.deepStrictEqual(options, {
  Random: constant('kTxOffsetSrcRandom'),
  RX: constant('kTxOffsetSrcRx'),
  Fixed: constant('kTxOffsetSrcCursor'),
}, 'each Offset src label sends the firmware value with its meaning');
console.log('PASS: offset src');
