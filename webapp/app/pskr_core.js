// Who hears me, without the page (docs/architecture/webapp.md, Who hears me): the MQTT 3.1.1 packets the card's
// WebSocket to PSKReporter's live feed sends and reads, the spots kept on the phone, and where the card was not
// listening while a spot of ours could have arrived. app.html owns the socket and draws what this returns;
// webapp/tests/test_pskr_core.js runs this file under node.
//
// Plain global, like swr_core.js: no module system.
var PskrCore = (function () {
  'use strict';

  var URL = 'wss://mqtt.pskreporter.info:1886';
  // The broker drops a client that sends nothing for 1.5x its keepalive (about 90 s at 60, measured 2026-10-07),
  // so the page pings well inside that. A socket that has heard nothing, not even a ping reply, for DEAD_MS is
  // treated as gone: a phone that locked mid-session wakes with a socket that has not noticed yet.
  var KEEPALIVE_S = 60;
  var PING_MS = 30000;
  var DEAD_MS = 75000;
  // A transmission's spots reached the feed up to 561 s after it (2026-10-07, 20 m FT8), so the card listens
  // this long after our last transmission, and counts a gap only inside that window.
  var LAG_MS = 15 * 60000;
  var KEEP_MS = 24 * 3600000;
  var MAX_SPOTS = 5000;     // a busy station's day would not fit in localStorage; the newest are kept
  var MIN_GAP_MS = 60000;   // a reconnect's few seconds is not worth a row
  var TX_MERGE_MS = 120000; // transmissions this close together are one stretch of operating

  var CALL_RE = /^[A-Z0-9\/]{3,15}$/;

  // The feed writes a '/' in a call as '.', since '/' separates topic levels: F5OYA/P is F5OYA.P (2026-10-08).
  // Our call as the sender, any band, mode or receiver. null for a call that cannot be a topic level.
  function topicFor(call) {
    var c = String(call || '').trim().toUpperCase();
    return CALL_RE.test(c) ? 'pskr/filter/v2/+/+/' + c.replace(/\//g, '.') + '/#' : null;
  }

  // ---- packets ----
  var enc = new TextEncoder();
  var dec = new TextDecoder();

  function concat(parts) {
    var n = 0;
    parts.forEach(function (p) { n += p.length; });
    var out = new Uint8Array(n), at = 0;
    parts.forEach(function (p) { out.set(p, at); at += p.length; });
    return out;
  }
  function utf8(s) {
    var b = enc.encode(s);
    return concat([new Uint8Array([b.length >> 8, b.length & 255]), b]);
  }
  function packet(first, body) {
    var len = [], n = body.length;
    do {
      var d = n % 128;
      n = Math.floor(n / 128);
      len.push(n ? d | 128 : d);
    } while (n);
    return concat([new Uint8Array([first].concat(len)), body]);
  }
  // Protocol "MQTT" level 4 (3.1.1), clean session, no will, no credentials.
  function connectPacket(clientId) {
    var head = new Uint8Array([0, 4, 77, 81, 84, 84, 4, 2, KEEPALIVE_S >> 8, KEEPALIVE_S & 255]);
    return packet(0x10, concat([head, utf8(clientId)]));
  }
  // QoS 0: the feed publishes at QoS 0 anyway, and nothing is kept for a client that is away.
  function subscribePacket(packetId, topic) {
    return packet(0x82, concat([new Uint8Array([packetId >> 8, packetId & 255]), utf8(topic), new Uint8Array([0])]));
  }
  var PINGREQ = new Uint8Array([0xC0, 0]);
  var DISCONNECT = new Uint8Array([0xE0, 0]);
  var CONNACK = 2, PUBLISH = 3, SUBACK = 9, PINGRESP = 13;

  // A WebSocket message may hold several packets or part of one (MQTT 3.1.1 §6), so bytes wait in the reader
  // until a whole packet is in. Returns the whole packets as {type, flags, body}.
  function newReader() { return { buf: new Uint8Array(0) }; }
  function readPackets(reader, bytes) {
    var buf = concat([reader.buf, bytes]), out = [], i = 0;
    for (;;) {
      if (i >= buf.length) break;
      var j = i + 1, len = 0, mult = 1, d, count = 0;
      do {
        if (j >= buf.length) { reader.buf = buf.slice(i); return out; }
        d = buf[j++];
        len += (d & 127) * mult;
        mult *= 128;
        if (++count === 4 && (d & 128)) throw new Error('malformed MQTT length');
      } while (d & 128);
      if (j + len > buf.length) break;
      out.push({ type: buf[i] >> 4, flags: buf[i] & 15, body: buf.slice(j, j + len) });
      i = j + len;
    }
    reader.buf = buf.slice(i);
    return out;
  }
  function publishOf(p) {
    var tl = (p.body[0] << 8) | p.body[1], off = 2 + tl;
    if ((p.flags >> 1) & 3) off += 2; // a packet id follows the topic at QoS 1 or 2
    return { topic: dec.decode(p.body.subarray(2, 2 + tl)), payload: dec.decode(p.body.subarray(off)) };
  }

  // ---- spots ----
  // One message is one spot, as JSON (https://www.mqtt.pskreporter.info/). t_tx is the start of the transmission,
  // normalised by the feed; t varies with the receiver's software, so it is only the fallback. The sender is
  // checked against our call as well as the topic: a spot of anyone else must never show as hearing us.
  function spotOf(payload, call) {
    var j;
    try { j = JSON.parse(payload); } catch (e) { return null; }
    if (!j || !j.rc || String(j.sc || '').toUpperCase() !== String(call).toUpperCase()) return null;
    var t = Number(j.t_tx || j.t);
    if (!isFinite(t) || t <= 0) return null;
    return {
      rx: String(j.rc).toUpperCase(), grid: String(j.rl || ''), band: String(j.b || ''), mode: String(j.md || ''),
      snr: typeof j.rp === 'number' ? j.rp : null, hz: Number(j.f) || 0, t: t * 1000
    };
  }
  // One receiver hearing one transmission: the same report can reach the feed twice.
  function spotKey(s) { return s.rx + '|' + s.band + '|' + s.mode + '|' + s.t; }

  // ---- what the phone keeps, per call ----
  // spots newest first; listen and tx are [from, to] ms ranges, oldest first.
  function newState(call) { return { v: 1, call: call, spots: [], listen: [], tx: [] }; }
  // What localStorage held, if it is this call's and this shape; otherwise a fresh start.
  function restore(saved, call) {
    var ok = saved && saved.v === 1 && saved.call === call &&
      Array.isArray(saved.spots) && Array.isArray(saved.listen) && Array.isArray(saved.tx);
    return ok ? saved : newState(call);
  }
  function addSpots(state, spots) {
    var have = {}, added = 0;
    state.spots.forEach(function (s) { have[spotKey(s)] = true; });
    spots.forEach(function (s) {
      var k = spotKey(s);
      if (have[k]) return;
      have[k] = true;
      state.spots.push(s);
      added++;
    });
    if (added) state.spots.sort(function (a, b) { return b.t - a.t; });
    if (state.spots.length > MAX_SPOTS) state.spots.length = MAX_SPOTS;
    return added;
  }
  function noteTx(state, ms) {
    var last = state.tx[state.tx.length - 1];
    if (last && ms - last[1] <= TX_MERGE_MS) last[1] = Math.max(last[1], ms);
    else state.tx.push([ms, ms]);
  }
  // A new subscription starts a range; each packet heard on it moves the range's end.
  function listenStart(state, ms) { state.listen.push([ms, ms]); }
  function listenAlive(state, ms) {
    var last = state.listen[state.listen.length - 1];
    if (last) last[1] = Math.max(last[1], ms);
  }
  function prune(state, now) {
    var cut = now - KEEP_MS;
    state.spots = state.spots.filter(function (s) { return s.t >= cut; });
    state.listen = state.listen.filter(function (r) { return r[1] >= cut; });
    state.tx = state.tx.filter(function (r) { return r[1] >= cut; });
  }

  // Whether a spot of ours could still arrive: we transmitted within LAG_MS.
  function wanted(state, now) {
    var last = state.tx[state.tx.length - 1];
    return !!last && now - last[1] < LAG_MS;
  }

  // Where a spot of ours could have arrived while the card was not subscribed: each stretch of transmitting
  // through LAG_MS after it, minus the listening ranges. Newest first, as {from, to}.
  function gaps(state, now) {
    var need = [];
    state.tx.forEach(function (r) {
      var a = r[0], b = Math.min(r[1] + LAG_MS, now), last = need[need.length - 1];
      if (b <= a) return;
      if (last && a <= last[1]) last[1] = Math.max(last[1], b);
      else need.push([a, b]);
    });
    var listen = state.listen.slice().sort(function (x, y) { return x[0] - y[0]; });
    var out = [];
    need.forEach(function (n) {
      var cur = n[0];
      listen.forEach(function (l) {
        if (l[1] <= cur || l[0] >= n[1]) return;
        if (l[0] > cur) out.push([cur, l[0]]);
        cur = Math.max(cur, l[1]);
      });
      if (cur < n[1]) out.push([cur, n[1]]);
    });
    return out.filter(function (g) { return g[1] - g[0] >= MIN_GAP_MS; })
      .map(function (g) { return { from: g[0], to: g[1] }; })
      .reverse();
  }

  var HOUR_MS = 3600000;

  // '160m' before '20m' before '70cm': the order the app's band grid uses (longest wavelength first).
  function bandMetres(b) {
    var m = /^(\d+(?:\.\d+)?)(cm|m)$/i.exec(String(b || ''));
    return m ? (m[2].toLowerCase() === 'cm' ? Number(m[1]) / 100 : Number(m[1])) : -1;
  }
  function sortBands(bands) {
    return bands.slice().sort(function (a, b) { return bandMetres(b) - bandMetres(a); });
  }
  // Every band in the stored spots, for the list's filter.
  function bandsIn(state) {
    var seen = {};
    state.spots.forEach(function (s) { if (s.band) seen[s.band] = true; });
    return sortBands(Object.keys(seen));
  }

  // The last hour at a glance: distinct receivers, the bands they heard us on, and the farthest of them.
  // distKm(grid) is the page's own great-circle distance from our grid, null when either grid is unknown.
  function hourFacts(state, now, distKm) {
    var seen = {}, bands = {}, far = null;
    state.spots.forEach(function (s) {
      if (now - s.t > HOUR_MS) return;
      seen[s.rx] = true;
      if (s.band) bands[s.band] = true;
      var km = s.grid ? distKm(s.grid) : null;
      if (km != null && (!far || km > far.km)) far = { rx: s.rx, grid: s.grid, km: km };
    });
    return { stations: Object.keys(seen).length, bands: sortBands(Object.keys(bands)), farthest: far };
  }

  // One row per receiver and band: its newest report, how many it sent, and its distance. opts.band keeps one
  // band ('' for all); opts.sort is 'time' (newest first), 'distance' (farthest first) or 'snr' (loudest first),
  // an unknown value sorting last, ties newest first.
  function receivers(state, opts) {
    var by = {}, list = [];
    state.spots.forEach(function (s) { // newest first
      if (opts.band && s.band !== opts.band) return;
      var k = s.rx + '|' + s.band, r = by[k];
      if (!r) {
        r = by[k] = { rx: s.rx, grid: s.grid, band: s.band, snr: s.snr, t: s.t, count: 0, km: null };
        list.push(r);
      }
      r.count++;
      if (!r.grid && s.grid) r.grid = s.grid; // a newer report without a locator still has an older one's
    });
    list.forEach(function (r) { r.km = r.grid && opts.distKm ? opts.distKm(r.grid) : null; });
    var key = { distance: 'km', snr: 'snr' }[opts.sort] || 't';
    list.sort(function (a, b) {
      var x = a[key], y = b[key];
      if (x == null || y == null) return x == null && y == null ? b.t - a.t : (x == null ? 1 : -1);
      return y - x || b.t - a.t;
    });
    return list;
  }

  // The list: up to `limit` receiver rows (receivers() above). Sorted by time, a gap row sits wherever the card
  // was not listening; a gap older than the oldest row shown is left off with the rows that did not fit.
  function rows(state, now, opts, limit) {
    var all = receivers(state, opts), shown = all.slice(0, limit), more = all.length - shown.length;
    var list = shown.map(function (r) { return { rx: r, t: r.t }; });
    if ((opts.sort || 'time') === 'time') {
      var floor = more > 0 && shown.length ? shown[shown.length - 1].t : -Infinity;
      gaps(state, now).forEach(function (g) { if (g.to >= floor) list.push({ gap: g, t: g.to }); });
      list.sort(function (a, b) { return b.t - a.t; });
    }
    return { rows: list, more: more };
  }

  // What one station reported of us, a row per band, newest first: for its call card.
  function heardBy(state, call) {
    if (!state || !call) return [];
    var c = String(call).toUpperCase();
    return receivers({ spots: state.spots.filter(function (s) { return s.rx === c; }) }, { sort: 'time' })
      .map(function (r) { return { band: r.band, snr: r.snr, t: r.t, count: r.count, grid: r.grid }; });
  }

  function hm(ms) {
    var d = new Date(ms);
    return String(d.getUTCHours()).padStart(2, '0') + ':' + String(d.getUTCMinutes()).padStart(2, '0');
  }
  // "14:10–14:25z", or "since 14:10z" for a gap still open now.
  function gapLabel(g, now) {
    return now - g.to < MIN_GAP_MS ? 'since ' + hm(g.from) + 'z' : hm(g.from) + '–' + hm(g.to) + 'z';
  }

  return {
    URL: URL, PING_MS: PING_MS, DEAD_MS: DEAD_MS, LAG_MS: LAG_MS, KEEP_MS: KEEP_MS, MAX_SPOTS: MAX_SPOTS,
    MIN_GAP_MS: MIN_GAP_MS, TX_MERGE_MS: TX_MERGE_MS,
    CONNACK: CONNACK, PUBLISH: PUBLISH, SUBACK: SUBACK, PINGRESP: PINGRESP, PINGREQ: PINGREQ, DISCONNECT: DISCONNECT,
    topicFor: topicFor, connectPacket: connectPacket, subscribePacket: subscribePacket,
    newReader: newReader, readPackets: readPackets, publishOf: publishOf,
    spotOf: spotOf, spotKey: spotKey,
    newState: newState, restore: restore, addSpots: addSpots, noteTx: noteTx, listenStart: listenStart,
    listenAlive: listenAlive, prune: prune, wanted: wanted, gaps: gaps, rows: rows,
    sortBands: sortBands, bandsIn: bandsIn, hourFacts: hourFacts, receivers: receivers, heardBy: heardBy, gapLabel: gapLabel
  };
})();
