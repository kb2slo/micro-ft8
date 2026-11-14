#!/usr/bin/env node
// worker.js's /download/ route (docs/RELEASE_PROCESS.md, Public Cloudflare mirror): which keys of
// the downloads bucket it serves, against a stand-in bucket. Not Cloudflare's own behaviour.
'use strict';
const assert = require('assert');
const path = require('path');
const { pathToFileURL } = require('url');

const STORED = {
  'manifest.json': '{}',
  'manifest-stable.json': '{}',
  'microft8-poe-p4-dev.bin': 'p4',
  'ota/20261114-microft8-poe-p4-ota-c1a53b5.bin': 'dev ota',
  'ota/microft8-poe-p4-ota-0.1.0.bin': 'stable ota',
  'app/0.1.0/manifest.json': '{"assets":[]}',
  'app/0.1.0/app.html': 'gz',
  // In the bucket, and still not served: nothing this route's rule names.
  'app/0.1.0/sub/app.html': 'nested',
  'app/latest/app.html': 'not a release',
  'ota/sub/x.bin': 'nested',
  'private/notes.txt': 'other',
};
const asked = [];
const env = {
  DOWNLOADS: {
    get: async (key) => {
      asked.push(key);
      if (!(key in STORED)) return null;
      return { body: STORED[key], httpEtag: '"e"', writeHttpMetadata() {} };
    },
  },
};

(async function () {
  const worker = (await import(pathToFileURL(path.join(__dirname, '..', 'worker.js')).href)).default;
  const get = (p, method) => worker.fetch(new Request('https://micro-ft8.kalikstein.com' + p, { method: method || 'GET' }), env);

  for (const key of ['manifest.json', 'manifest-stable.json', 'microft8-poe-p4-dev.bin',
    'ota/20261114-microft8-poe-p4-ota-c1a53b5.bin', 'ota/microft8-poe-p4-ota-0.1.0.bin',
    'app/0.1.0/manifest.json', 'app/0.1.0/app.html']) {
    const r = await get('/download/' + key);
    assert.strictEqual(r.status, 200, key);
    assert.strictEqual(await r.text(), STORED[key], key);
    // The app reads these from another origin, the sidekick's.
    assert.strictEqual(r.headers.get('access-control-allow-origin'), '*', key);
  }
  assert.strictEqual((await get('/download/app/0.1.0/app.html', 'HEAD')).status, 200);
  assert.strictEqual((await get('/download/app/0.1.0/manifest.json')).headers.get('content-type'), 'application/json; charset=utf-8');
  assert.strictEqual((await get('/download/app/0.1.0/manifest.json')).headers.get('content-disposition'), null);

  // Never asked of the bucket at all: nesting, traversal, and an app path that is not a release's.
  asked.length = 0;
  for (const p of ['app/0.1.0/sub/app.html', 'app/latest/app.html', 'app/0.1.0/', 'app/0.1/app.html', 'app/0.1.0/..%2Fmanifest.json',
    'ota/sub/x.bin', 'ota/', 'private/notes.txt', '..%2Fsecret', 'app%2F0.1.0%2Fsub%2Fx', '', '%E0%A4%A']) {
    assert.strictEqual((await get('/download/' + p)).status, 404, p);
  }
  assert.deepStrictEqual(asked, [], 'refused before the bucket');
  // A key the rule allows and the bucket lacks.
  assert.strictEqual((await get('/download/app/0.2.0/app.html')).status, 404);
  assert.strictEqual((await get('/download/manifest.json', 'POST')).status, 405);
  assert.strictEqual((await get('/elsewhere')).status, 404);
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
