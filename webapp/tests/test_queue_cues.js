#!/usr/bin/env node
// The queue in the decodes (webapp.md, Decode rows and tap to reply): runs app.html's own
// queueInvolvement() and involvementLabel(). A decode names a queued station when it is the
// sender (calling CQ again, working or done with someone else, or sending to us) or when someone
// else calls it; anything else, and our CQ entry, is not a match.
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
const varDecl = (name) => {
  const m = new RegExp('var ' + name + ' = [\\s\\S]*?;\\n').exec(src);
  assert(m, 'var not found in app.html: ' + name);
  return m[0];
};

const sb = {};
vm.createContext(sb);
vm.runInContext([varDecl('GRID_TOKEN_RE'), varDecl('NON_CALL_WORDS')].concat(
  ['looksLikeCallsign', 'extractSenderCall', 'queueInvolvement', 'involvementLabel']
    .map((n) => extractFunction(src, n))).join('\n'), sb);

const me = 'KB2SLO';
const q = ['K9ABC', 'N8ABC'];
const inv = (x) => {
  const v = sb.queueInvolvement(x, q, me);
  return v ? v.call + ' ' + sb.involvementLabel(v) : null;
};

assert.strictEqual(inv('CQ K9ABC EN52'), 'K9ABC CQ again', 'a queued station calling CQ again');
assert.strictEqual(inv('CQ POTA K9ABC EN52'), 'K9ABC CQ again', 'with a modifier');
assert.strictEqual(inv('W1XYZ K9ABC -12'), 'K9ABC Working W1XYZ', 'working someone else');
assert.strictEqual(inv('W1XYZ K9ABC R-07'), 'K9ABC Working W1XYZ');
assert.strictEqual(inv('W1XYZ K9ABC RR73'), 'K9ABC Done with W1XYZ', 'signing off with someone else');
assert.strictEqual(inv('W1XYZ K9ABC 73'), 'K9ABC Done with W1XYZ');
assert.strictEqual(inv('K9ABC W1XYZ FN42'), 'K9ABC Called by W1XYZ', 'someone else calling it');
assert.strictEqual(inv('KB2SLO K9ABC -09'), 'K9ABC → you', 'sending to us');
assert.strictEqual(inv('K9ABC KB2SLO R-11'), null, 'our own message to it is not a decode about it');
assert.strictEqual(inv('W1XYZ W2DEF -05'), null, 'stations not in the queue');
assert.strictEqual(inv('CQ W1XYZ FN42'), null);
assert.strictEqual(inv('<K9ABC/P> W1XYZ RR73'), null, 'a compound form is another call');
assert.strictEqual(sb.queueInvolvement('W1XYZ K9ABC -12', [], me), null, 'nothing queued');
assert.strictEqual(sb.queueInvolvement('CQ K9ABC EN52', ['CQ'], me), null, 'the CQ entry is not a station');

console.log('PASS: queue cues');
