#!/usr/bin/env node
// worker.js's /pskr/ route and pskr_report.js (docs/architecture/system.md, Third-party services): what a
// post must be, what the gate lets go and when, and the IPFIX message PSKReporter is sent, byte for byte
// against one written out by hand from pskdev.html and read back by a reader of its own. PSKReporter, the
// Durable Object's storage and the TCP socket are stand-ins here; not Cloudflare's behaviour, nor
// PSKReporter's.
'use strict';
const assert = require('assert');
const path = require('path');
const { pathToFileURL } = require('url');

const NOW_S = 1795000000; // 2026-11-18
const MIN = 60 * 1000;

(async function () {
  const P = await import(pathToFileURL(path.join(__dirname, '..', 'pskr_report.js')).href);
  const W = await import(pathToFileURL(path.join(__dirname, '..', 'worker.js')).href);
  const spot = (c, more) => Object.assign({ c, g: 'FN42', f: 14075500, s: -12, m: 'FT8', t: NOW_S - 30 }, more);
  const post = (spots, more) => Object.assign({ call: 'KB2SLO', grid: 'EM10cg', app: '0.1.0+0c5ccec', spots }, more);

  // ---- A post, checked ----
  {
    const ok = P.checkReport(post([spot('K1ABC'), spot('w1aw', { g: '' })]), NOW_S);
    assert.ok(ok.ok);
    assert.deepStrictEqual(ok.report.spots.map((s) => [s.c, s.g]), [['K1ABC', 'FN42'], ['W1AW', '']], 'calls upper-cased');
    assert.strictEqual(ok.report.grid, 'EM10cg', 'the locator as PSKReporter shows one');
    assert.strictEqual(P.checkReport(post([spot('K1ABC')], { grid: 'em10CG' }), NOW_S).report.grid, 'EM10cg');
    for (const [why, body, error] of [
      ['no call', post([spot('K1ABC')], { call: '' }), 'bad_call'],
      ['a word for a call', post([spot('K1ABC')], { call: 'HELLO' }), 'bad_call'],
      ['no grid', post([spot('K1ABC')], { grid: '' }), 'bad_grid'],
      ['a grid out of range', post([spot('K1ABC')], { grid: 'ZZ99' }), 'bad_grid'],
      ['no spots', post([]), 'bad_spots'],
      ['too many', post(Array.from({ length: P.MAX_SPOTS + 1 }, (_, i) => spot('K' + i + 'AB'))), 'bad_spots'],
      ['not an object', null, 'bad_request'],
    ]) {
      assert.deepStrictEqual(P.checkReport(body, NOW_S), { ok: false, error }, why);
    }
    // A spot that is not one is dropped; the rest of the post stands.
    const mixed = P.checkReport(post([
      spot('K1ABC'),
      spot('KB2SLO'),                             // ourselves
      spot('W2XYZ', { f: 15000000 }),             // out of the bands
      spot('W3XYZ', { s: 99 }),                   // no SNR this is
      spot('W4XYZ', { m: 'JT65' }),
      spot('W5XYZ', { t: NOW_S - 3601 }),         // older than PSKReporter keeps
      spot('W6XYZ', { t: NOW_S + 61 }),           // the future
      spot('W7XYZ', { f: 14075500.5 }),
      spot('W8XYZ', { g: 'ZZ12' }),               // kept, without its locator
      'K9ABC',
    ]), NOW_S);
    assert.deepStrictEqual(mixed.report.spots.map((s) => [s.c, s.g]), [['K1ABC', 'FN42'], ['W8XYZ', '']]);
    // One spot a sender, its latest.
    const twice = P.checkReport(post([spot('K1ABC', { t: NOW_S - 90, s: -3 }), spot('K1ABC', { t: NOW_S - 15, s: -20 })]), NOW_S);
    assert.deepStrictEqual(twice.report.spots.map((s) => s.s), [-20]);
    assert.strictEqual(P.callOf('PJ4/K1ABC'), 'PJ4/K1ABC');
    assert.strictEqual(P.callOf('K1ABC/P'), 'K1ABC/P');
    assert.strictEqual(P.callOf('K1ABC/'), '');
    assert.strictEqual(P.bandOf(7074000), '40m');
    assert.strictEqual(P.bandOf(21140000), '15m');
  }

  // ---- The gate ----
  {
    const t0 = NOW_S * 1000;
    const a = P.gate(null, [spot('K1ABC'), spot('W1AW')], t0);
    assert.strictEqual(a.action, 'send', 'a fresh station sends at once');
    assert.strictEqual(a.seq, 0);
    assert.strictEqual(a.retryMs, P.WINDOW_MS);
    assert.strictEqual(a.state.seq, 2, 'the sequence number counts records');

    const b = P.gate(a.state, [spot('N0CALL1')], t0 + 2 * MIN);
    assert.deepStrictEqual([b.action, b.retryMs], ['wait', 3 * MIN], 'a second message inside five minutes waits');
    assert.strictEqual(b.state.last, a.state.last);

    const c = P.gate(a.state, [spot('K1ABC'), spot('W1AW')], t0 + 5 * MIN);
    assert.strictEqual(c.action, 'none', 'what was reported is not reported again');
    assert.strictEqual(c.state.last, a.state.last, 'and opens no window');

    // Two pages: the second's post, after the window, carries only what the first did not send.
    const d = P.gate(a.state, [spot('K1ABC'), spot('VE3PFW')], t0 + 5 * MIN);
    assert.deepStrictEqual(d.spots.map((s) => s.c), ['VE3PFW']);
    assert.strictEqual(d.seq, 2);

    const e = P.gate(d.state, [spot('K1ABC', { f: 7074500 }), spot('K1ABC', { m: 'FT4', f: 14081000 })], t0 + 11 * MIN);
    assert.strictEqual(e.spots.length, 2, 'a sender heard on another band, or in another mode, is reported');

    const f = P.gate(e.state, [spot('W1AW')], t0 + 59 * MIN);
    assert.strictEqual(f.action, 'none', 'within the hour, once');
    const g = P.gate(e.state, [spot('W1AW')], t0 + 60 * MIN);
    assert.strictEqual(g.action, 'send', 'after an hour, again');
    assert.ok(!('K1ABC 20m FT8' in g.state.sent) || g.state.sent['K1ABC 20m FT8'] > t0, 'an hour-old sender is dropped');
    assert.strictEqual(P.gate({ last: 0, seq: 0xffffffff, sent: {} }, [spot('K1ABC')], t0).state.seq, 0, 'it wraps as IPFIX does');
  }

  // ---- The message, byte for byte ----
  {
    const bytes = P.encodeMessage({
      call: 'KB2SLO', grid: 'EM10cg', software: 'micro-FT8 0.1.0',
      spots: [{ c: 'K1ABC', g: 'FN42', f: 14075500, s: -12, m: 'FT8', t: 0x6afd3b00 },
        { c: 'VE3PFW', g: '', f: 21141000, s: 3, m: 'FT4', t: 0x6afd3b0f }],
      seq: 7, domain: 0x01020304, exportS: 0x6afd3c00,
    });
    const hex = (s) => s.replace(/\s+/g, '');
    const str = (s) => (s.length.toString(16).padStart(2, '0') + Buffer.from(s).toString('hex'));
    const want = hex(
      // Header: version 10, length, export time, sequence, observation domain.
      '000a 00c8 6afd3c00 00000007 01020304' +
      // The receiver's options template: set 3, template 0x50e2, three fields, one of them scope, padded.
      '0003 0024 50e2 0003 0001 8002ffff0000768f 8004ffff0000768f 8008ffff0000768f 0000' +
      // A sender's template: set 2, template 0x50e3, seven fields.
      '0002 003c 50e3 0007 8001ffff0000768f 800500040000768f 800600010000768f 800affff0000768f' +
      ' 8003ffff0000768f 800b00010000768f 00960004' +
      // The receiver: call, locator, software, padded to four.
      '50e2 0024 ' + str('KB2SLO') + str('EM10cg') + str('micro-FT8 0.1.0') + '0000' +
      // The spots: call, Hz, SNR, mode, locator, informationSource 1, time; padded.
      '50e3 0034 ' + str('K1ABC') + '00d6c66c f4 ' + str('FT8') + str('FN42') + '01 6afd3b00' +
      str('VE3PFW') + '0142 9608 03 ' + str('FT4') + '00 01 6afd3b0f' + '00');
    assert.strictEqual(Buffer.from(bytes).toString('hex'), want);

    // And read back as IPFIX, by the templates it carries.
    const read = readIpfix(bytes);
    assert.strictEqual(read.length, bytes.length);
    assert.deepStrictEqual(read.records['20706'], [{ 2: 'KB2SLO', 4: 'EM10cg', 8: 'micro-FT8 0.1.0' }]);
    assert.deepStrictEqual(read.records['20707'], [
      { 1: 'K1ABC', 5: 14075500, 6: -12, 10: 'FT8', 3: 'FN42', 11: 1, 150: 0x6afd3b00 },
      { 1: 'VE3PFW', 5: 21141000, 6: 3, 10: 'FT4', 3: '', 11: 1, 150: 0x6afd3b0f }]);

    // A long string is cut to IPFIX's one-byte length.
    const long = readIpfix(P.encodeMessage({ call: 'KB2SLO', grid: 'EM10', software: 'x'.repeat(300), spots: [],
      seq: 0, domain: 0, exportS: 0 }));
    assert.strictEqual(long.records['20706'][0][8].length, 254);
    assert.strictEqual(P.domainOf('KB2SLO'), P.domainOf('KB2SLO'));
    assert.notStrictEqual(P.domainOf('KB2SLO'), P.domainOf('K1ABC'), 'each station its own domain');
  }

  // ---- The route ----
  {
    const sent = [];
    let refuse = false;
    const storage = new Map();
    const gate = new P.PskrGate({ storage: { get: async (k) => storage.get(k), put: async (k, v) => { storage.set(k, structuredClone(v)); } } });
    const points = [];
    const env = {
      PSKR_GATE: { idFromName: (n) => n, get: () => ({ fetch: (u, init) => gate.fetch(new Request(u, init)) }) },
      PSKR_REPORTS: { writeDataPoint: (p) => points.push(p) },
      PSKR_CONNECT: ({ hostname, port }) => {
        const out = { hostname, port, chunks: [], closed: false };
        sent.push(out);
        return {
          opened: refuse ? Promise.reject(new Error('refused')) : Promise.resolve(),
          writable: new WritableStream({ write(c) { out.chunks.push(c); }, close() { out.closed = true; } }),
          close: async () => {},
        };
      },
    };
    const call = async (body, method, e) => {
      const r = await W.default.fetch(new Request('https://micro-ft8.kalikstein.com/pskr/report', {
        method: method || 'POST', body: method === 'GET' ? undefined : (typeof body === 'string' ? body : JSON.stringify(body)),
      }), e || env);
      assert.strictEqual(r.headers.get('access-control-allow-origin'), '*', 'the sidekick\'s page may read it');
      assert.strictEqual(r.headers.get('cache-control'), 'no-store');
      return Object.assign(JSON.parse(await r.text()), { http: r.status });
    };
    const nowS = () => Math.floor(Date.now() / 1000);
    const live = (c, more) => spot(c, Object.assign({ t: nowS() - 20 }, more));

    const first = await call(post([live('K1ABC'), live('W1AW')]));
    assert.deepStrictEqual(first, { ok: true, sent: 2, retry_in: P.WINDOW_MS, http: 200 });
    assert.strictEqual(sent.length, 1);
    assert.deepStrictEqual([sent[0].hostname, sent[0].port, sent[0].closed], ['report.pskreporter.info', 4739, true]);
    const msg = readIpfix(Buffer.concat(sent[0].chunks.map((c) => Buffer.from(c))));
    assert.deepStrictEqual(msg.records['20706'], [{ 2: 'KB2SLO', 4: 'EM10cg', 8: 'micro-FT8 0.1.0+0c5ccec' }]);
    assert.deepStrictEqual(msg.records['20707'].map((r) => r[1]), ['K1ABC', 'W1AW']);
    assert.ok(Math.abs(msg.exportS - nowS()) <= 1, 'the header carries the time it was sent');


    // A second page, at once: told when to come back, and nothing sent.
    const again = await call(post([live('K1ABC'), live('VE3PFW')]));
    assert.strictEqual(again.sent, 0);
    assert.ok(again.retry_in > 4.9 * MIN && again.retry_in <= P.WINDOW_MS, String(again.retry_in));
    assert.strictEqual(sent.length, 1);

    // Each post counted, by what came of it.
    assert.deepStrictEqual(points.map((p) => p.blobs[5]), ['sent', 'wait']);
    assert.deepStrictEqual(points[0], { blobs: ['KB2SLO', 'EM10', '0.1.0+0c5ccec', '20m', 'FT8', 'sent'], doubles: [2, 2], indexes: ['KB2SLO'] });

    // PSKReporter unreachable: the page is told to move on.
    storage.clear();
    refuse = true;
    assert.deepStrictEqual(await call(post([live('K1ABC')])), { ok: false, error: 'unreachable', retry_in: P.WINDOW_MS, http: 200 });
    assert.strictEqual(points[points.length - 1].blobs[5], 'send_failed');
    refuse = false;

    // Nothing left once checked: no gate asked, nothing sent.
    const before = sent.length;
    assert.deepStrictEqual(await call(post([live('KB2SLO')])), { ok: true, sent: 0, retry_in: 0, http: 200 });
    assert.strictEqual(sent.length, before);

    assert.strictEqual((await call(null, 'GET')).http, 405);
    assert.deepStrictEqual(await call('{nope'), { ok: false, error: 'bad_request', http: 400 });
    assert.deepStrictEqual(await call(post([live('K1ABC')], { call: '' })), { ok: false, error: 'bad_call', http: 400 });
    assert.deepStrictEqual(await call('x'.repeat(P.MAX_BODY + 1)), { ok: false, error: 'too_large', http: 413 });
    assert.deepStrictEqual(await call(post([live('K1ABC')]), 'POST', {}), { ok: false, error: 'unavailable', http: 503 });

    // The most a post carries is one message, well inside IPFIX's 65,535 bytes.
    storage.clear();
    const big = Array.from({ length: P.MAX_SPOTS }, (_, i) => live('K' + i + 'XYZ/QRP', { g: 'FN42' }));
    assert.strictEqual((await call(post(big))).sent, P.MAX_SPOTS);
    const one = Buffer.concat(sent[sent.length - 1].chunks.map((c) => Buffer.from(c)));
    const whole = readIpfix(one);
    assert.strictEqual(whole.records['20707'].length, P.MAX_SPOTS, 'every spot, in one message');
    assert.ok(one.length < 8 * 1024, one.length + ' bytes');

    assert.strictEqual(W.pskrSoftware('b1322+0c5ccec'), 'micro-FT8 b1322+0c5ccec');
    assert.strictEqual(W.pskrSoftware('37f14119-dirty 2026-11-23 14:57:39'), 'micro-FT8 37f14119-dirty');
    assert.strictEqual(W.pskrSoftware(undefined), 'micro-FT8');
  }

  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });

// An IPFIX reader of its own (RFC 7011): the header, then each set by its templates.
function readIpfix(buf) {
  const b = Buffer.from(buf);
  assert.strictEqual(b.readUInt16BE(0), 10, 'IPFIX');
  const length = b.readUInt16BE(2);
  assert.strictEqual(length, b.length, 'the header\'s length is the message\'s');
  const out = { length, exportS: b.readUInt32BE(4), seq: b.readUInt32BE(8), domain: b.readUInt32BE(12), records: {} };
  const templates = {};
  let at = 16;
  while (at < length) {
    const id = b.readUInt16BE(at), len = b.readUInt16BE(at + 2);
    assert.ok(len >= 4 && len % 4 === 0 && at + len <= length, 'a set padded to four, inside the message');
    const end = at + len;
    let p = at + 4;
    if (id === 2 || id === 3) {
      const tid = b.readUInt16BE(p), n = b.readUInt16BE(p + 2);
      p += id === 3 ? 6 : 4;
      const fields = [];
      for (let i = 0; i < n; i++) {
        let fid = b.readUInt16BE(p);
        const flen = b.readUInt16BE(p + 2);
        p += 4;
        if (fid & 0x8000) {
          assert.strictEqual(b.readUInt32BE(p), 30351, 'PSKReporter\'s enterprise');
          fid &= 0x7fff;
          p += 4;
        }
        fields.push([fid, flen]);
      }
      templates[tid] = fields;
    } else {
      const fields = templates[id];
      assert.ok(fields, 'a data set after its template');
      out.records[id] = [];
      while (end - p >= 4 || (end - p > 0 && b[p] !== 0)) {
        const rec = {};
        for (const [fid, flen] of fields) {
          if (flen === 0xffff) {
            const n = b[p];
            rec[fid] = b.slice(p + 1, p + 1 + n).toString('utf8');
            p += 1 + n;
          } else if (fid === 6) {
            rec[fid] = b.readInt8(p);
            p += flen;
          } else {
            rec[fid] = b.readUIntBE(p, flen);
            p += flen;
          }
        }
        out.records[id].push(rec);
        if (end - p < 4 && b.slice(p, end).every((x) => x === 0)) break;
      }
    }
    at = end;
  }
  return out;
}
