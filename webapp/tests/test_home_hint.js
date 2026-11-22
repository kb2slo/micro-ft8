// When the one-time Add to Home Screen hint is due (homeScreenHintDue(), webapp/app/app.html; docs/architecture/
// webapp.md, Home Screen): only in an iPhone's Safari tab (navigator.standalone false), until Got it is tapped.

'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const src = fs.readFileSync(path.join(__dirname, '../app/app.html'), 'utf8');
const fn = /function homeScreenHintDue\([^)]*\) \{[^}]*\}/.exec(src);
assert(fn, 'homeScreenHintDue not found in app.html');
const sb = {};
vm.createContext(sb);
vm.runInContext(fn[0], sb);

assert.strictEqual(sb.homeScreenHintDue(false, false), true, 'an iPhone Safari tab, not dismissed: shown');
assert.strictEqual(sb.homeScreenHintDue(false, true), false, 'Got it tapped: not again');
assert.strictEqual(sb.homeScreenHintDue(true, false), false, 'opened from the Home Screen: nothing to say');
assert.strictEqual(sb.homeScreenHintDue(undefined, false), false, 'not iOS Safari (no navigator.standalone): never');

console.log('PASS: Home Screen hint');
