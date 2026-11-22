// The pairing token carried from one address of the sidekick to another (sidekick/main/web/pairing.js,
// carryLink() and the fragment it takes at load; docs/architecture/sidekick.md, Pairing). A phone that
// saves the home WiFi at 192.168.4.1 opens the Saved page's link at micro-ft8.local, a different origin
// with its own storage: the link carries the token in its fragment, and the page that opens stores it
// and clears it from the address before anything reads the hash.

'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const SRC = fs.readFileSync(path.join(__dirname, '../../sidekick/main/web/pairing.js'), 'utf8');
const TOKEN = '0123456789abcdef0123456789abcdef';

// Loads pairing.js into a fresh window at `href`, with `stored` already in its storage.
function load(href, stored) {
  const url = new URL(href);
  const store = new Map(stored ? [['microft8_token', stored]] : []);
  const replaced = [];
  const win = {
    localStorage: {
      getItem: (k) => (store.has(k) ? store.get(k) : null),
      setItem: (k, v) => store.set(k, String(v)),
      removeItem: (k) => store.delete(k),
    },
    location: { hash: url.hash, pathname: url.pathname, search: url.search },
    history: { replaceState: (s, t, u) => { replaced.push(u); } },
    fetch: () => Promise.reject(new Error('no network in this test')),
    setTimeout: setTimeout,
  };
  win.window = win;
  vm.createContext(win);
  vm.runInContext(SRC, win, { filename: 'pairing.js' });
  return { win, store, replaced };
}

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

check('the Saved page link carries the token in its fragment', () => {
  const { win } = load('http://192.168.4.1/provision', TOKEN);
  assert.strictEqual(win.MicroFT8Pairing.carryLink('http://micro-ft8.local/app.html'),
                     'http://micro-ft8.local/app.html#pair=' + TOKEN);
  assert.strictEqual(win.MicroFT8Pairing.carryLink('http://micro-ft8.local/app.html#network'),
                     'http://micro-ft8.local/app.html#pair=' + TOKEN, 'one fragment, the token');
});

check('with no token the link is unchanged', () => {
  const { win } = load('http://192.168.4.1/provision', null);
  assert.strictEqual(win.MicroFT8Pairing.carryLink('http://micro-ft8.local/app.html'),
                     'http://micro-ft8.local/app.html');
});

check('a page opened with the token stores it and clears it from the address', () => {
  const { win, replaced } = load('http://micro-ft8.local/app.html#pair=' + TOKEN, null);
  assert.strictEqual(win.MicroFT8Pairing.getToken(), TOKEN);
  assert.deepStrictEqual(replaced, ['/app.html'], 'the fragment is gone before the app reads the hash');
});

check('a carried token replaces one stored before', () => {
  const { win } = load('http://micro-ft8.local/app.html#pair=' + TOKEN, 'ffffffffffffffffffffffffffffffff');
  assert.strictEqual(win.MicroFT8Pairing.getToken(), TOKEN);
});

check('anything but 32 lowercase hex is not taken, and the address is left alone', () => {
  for (const hash of ['#pair=abc', '#pair=' + TOKEN.toUpperCase(), '#pair=' + TOKEN + '0', '#network']) {
    const { win, replaced } = load('http://micro-ft8.local/app.html' + hash, null);
    assert.strictEqual(win.MicroFT8Pairing.getToken(), '', hash);
    assert.deepStrictEqual(replaced, [], hash);
  }
});

if (failures > 0) {
  console.log(failures + ' check(s) failed');
  process.exit(1);
}
console.log('All pairing-carry checks passed');
