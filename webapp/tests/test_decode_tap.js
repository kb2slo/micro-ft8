#!/usr/bin/env node
// Tap a decode row to reply (webapp.md, Decode rows and tap to reply): runs app.html's own
// decodeRowReplyable() and decodeTapAction(). A row replies only when its message is replyable
// (isReplyableDecode()) on a slot still open (slotReplyOpen()), and not while a reply from it is
// sending or queued; a tap on its callsign or grid link is the link's, never a reply.
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

const sb = { lastCfg: {} };
vm.createContext(sb);
vm.runInContext(['slotReplyOpen', 'isReplyableDecode', 'decodeRowReplyable', 'decodeTapAction']
  .map((n) => extractFunction(src, n)).join('\n'), sb);

const now = Date.UTC(2026, 9, 25, 14, 2, 36);
const slot = (endMs, lenMs) => Object.assign([], { ts: endMs, lenMs: lenMs || 15000 });
const fresh = slot(now - 6000), previous = slot(now - 21000), old = slot(now - 31000);
const cq = { x: 'CQ POTA K4XYZ EM85', cq: true };
const rr73 = { x: 'W9ABC G4ABC RR73', cq: false };
const report = { x: 'KB2SLO VE3ABC R-09', cq: false, me: true };
const tap = (d, s, st) => sb.decodeRowReplyable(d, s, now, st);

assert(tap(cq, fresh), 'a CQ in the slot just ended');
assert(tap(cq, previous), 'and in the slot before it');
assert(!tap(cq, old), 'not past two slot lengths');
assert(tap(rr73, fresh), 'a closing RR73 is replyable');
assert(tap({ x: 'W9ABC G4ABC 73', cq: false }, fresh), 'and a 73');
assert(!tap(report, fresh), 'a report, even to us, is not a reply target');
assert(tap(cq, slot(now - 14000, 7500)) === true && !tap(cq, slot(now - 16000, 7500)), 'FT4 halves the window');

assert(!tap(cq, fresh, { phase: 'sending' }), 'not while its reply is sending');
assert(!tap(cq, fresh, { phase: 'queued' }), 'not once queued');
assert(tap(cq, fresh, { phase: 'refused' }), 'a refused reply can be tried again');

assert.strictEqual(sb.decodeTapAction(true, true), 'link', 'a callsign or grid link in a replyable row keeps its tap');
assert.strictEqual(sb.decodeTapAction(true, false), 'link');
assert.strictEqual(sb.decodeTapAction(false, true), 'reply', 'anywhere else on a replyable row replies');
assert.strictEqual(sb.decodeTapAction(false, false), 'none', 'a row that is not replyable does nothing');

// The row's markup carries the rule: a tappable row is a button with the callsign in its label,
// and the reply goes through the same checks as before (too old, another frequency).
assert(/role=button tabindex=0 aria-label="Reply to /.test(src), 'a replyable row is a button for assistive technology');
assert(/function tryReply\(id\) \{\s*var why = replyTooOld\(id\) \|\| replyFrequencyMismatch\(id\);/.test(src),
  'a tap is refused for an aged slot or another frequency, as Reply was');

console.log('PASS: decode tap to reply');
