#!/usr/bin/env node
// Settings › Station's line under Comment (docs/architecture/webapp.md, Settings › Station): app.html's
// own commentPreview() fills /Radio and /Grid as the main board does when it logs, so the default
// "micro-ft8 /Radio" no longer reads as the words a QSO will carry. /Radio is the radio's own first word
// for itself, the same rule as adif_radio_name() (components/qso_log/adif_logger.cpp, host_mock).
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');
const start = src.indexOf('function commentPreview(');
assert(start !== -1, 'commentPreview not found in app.html');
const end = src.indexOf('\n}\n', start) + 3;
const ctx = vm.createContext({});
vm.runInContext(src.slice(start, end), ctx);
const p = ctx.commentPreview;

assert.strictEqual(p('micro-ft8 /Radio', 'QMX 60-15m', 'EM10cg'), 'micro-ft8 QMX', "a new station's comment on a QMX");
assert.strictEqual(p('micro-ft8 /Radio', 'QMX+ 160-6m', 'EM10cg'), 'micro-ft8 QMX+', 'a QMX+ names itself');
assert.strictEqual(p('micro-ft8 /Radio', '', 'EM10cg'), 'micro-ft8 QMX', 'before the radio is read: QMX, as the main board logs');
assert.strictEqual(p('TNX /Radio /Grid', 'QMX 60-15m', 'EM10'), 'TNX QMX EM10', 'both macros, every time they appear');
assert.strictEqual(p('/Radio /Radio', 'QMX 60-15m', ''), 'QMX QMX');
assert.strictEqual(p('micro-ft8 / QMX', 'QMX 60-15m', 'EM10'), '', 'no macro: nothing to show, the comment is what logs');
assert.strictEqual(p('', 'QMX 60-15m', 'EM10'), '', 'an empty comment');

// The line sits under the Comment field.
const i = src.indexOf('<input id=comment1');
assert(i > 0 && src.indexOf('<p id=commentHint class=setnote></p>', i) - i < 200, 'the line is right under Comment');
console.log('PASS: comment hint');
