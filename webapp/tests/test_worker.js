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
  // In the bucket, and still not served: nothing this route's rule names.
  'app/0.1.0/app.html': 'a release\'s app, from before the app shipped in the sidekick\'s image',
  'ota/sub/x.bin': 'nested',
  'private/notes.txt': 'other',
};
const asked = [];
const counted = [];
const env = {
  DOWNLOAD_COUNTS: { writeDataPoint: (p) => { counted.push(p); } },
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
    'ota/20261114-microft8-poe-p4-ota-c1a53b5.bin', 'ota/microft8-poe-p4-ota-0.1.0.bin']) {
    const r = await get('/download/' + key);
    assert.strictEqual(r.status, 200, key);
    assert.strictEqual(await r.text(), STORED[key], key);
    // The app reads these from another origin, the sidekick's.
    assert.strictEqual(r.headers.get('access-control-allow-origin'), '*', key);
  }
  assert.strictEqual((await get('/download/ota/microft8-poe-p4-ota-0.1.0.bin', 'HEAD')).status, 200);
  assert.strictEqual((await get('/download/manifest.json')).headers.get('content-type'), 'application/json; charset=utf-8');
  assert.strictEqual((await get('/download/manifest.json')).headers.get('content-disposition'), null);

  // Never asked of the bucket at all: nesting and traversal.
  asked.length = 0;
  for (const p of ['app/0.1.0/app.html', 'app/0.1.0/', 'ota/..%2Fmanifest.json',
    'ota/sub/x.bin', 'ota/', 'private/notes.txt', '..%2Fsecret', 'app%2F0.1.0%2Fsub%2Fx', '', '%E0%A4%A']) {
    assert.strictEqual((await get('/download/' + p)).status, 404, p);
  }
  assert.deepStrictEqual(asked, [], 'refused before the bucket');
  // A key the rule allows and the bucket lacks.
  assert.strictEqual((await get('/download/ota/microft8-poe-p4-ota-0.2.0.bin')).status, 404);
  assert.strictEqual((await get('/download/manifest.json', 'POST')).status, 405);
  assert.strictEqual((await get('/elsewhere')).status, 404);

  // Download counts: one point per file served, by its key and by how it was fetched. A manifest,
  // a HEAD, a refusal and a missing file are not downloads.
  counted.length = 0;
  for (const p of ['manifest.json', 'manifest-stable.json', 'ota/microft8-poe-p4-ota-0.2.0.bin',
    'app/0.1.0/app.html', 'private/notes.txt']) await get('/download/' + p);
  await get('/download/microft8-poe-p4-dev.bin', 'HEAD');
  await get('/download/microft8-poe-p4-dev.bin', 'POST');
  assert.deepStrictEqual(counted, [], 'not downloads');
  await get('/download/microft8-poe-p4-dev.bin');
  await get('/download/microft8-poe-p4-dev.bin?via=usb');
  await get('/download/microft8-poe-p4-dev.bin?via=other');
  await get('/download/ota/microft8-poe-p4-ota-0.1.0.bin');
  assert.deepStrictEqual(counted, [
    { blobs: ['microft8-poe-p4-dev.bin', 'download'], indexes: ['microft8-poe-p4-dev.bin'] },
    { blobs: ['microft8-poe-p4-dev.bin', 'usb'], indexes: ['microft8-poe-p4-dev.bin'] },
    { blobs: ['microft8-poe-p4-dev.bin', 'download'], indexes: ['microft8-poe-p4-dev.bin'] },
    { blobs: ['ota/microft8-poe-p4-ota-0.1.0.bin', 'download'], indexes: ['ota/microft8-poe-p4-ota-0.1.0.bin'] },
  ]);
  // The query string names no other file, and a count that cannot be written still serves the file.
  assert.strictEqual(await (await get('/download/microft8-poe-p4-dev.bin?via=usb')).text(), 'p4');
  for (const counts of [undefined, { writeDataPoint() { throw new Error('refused'); } }]) {
    const r = await worker.fetch(new Request('https://micro-ft8.kalikstein.com/download/microft8-poe-p4-dev.bin'),
      { DOWNLOADS: env.DOWNLOADS, DOWNLOAD_COUNTS: counts });
    assert.strictEqual(r.status, 200);
    assert.strictEqual(await r.text(), 'p4');
  }
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
