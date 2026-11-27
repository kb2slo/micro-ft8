// Importing the operator's QRZ logbook into the station's log, without the page (docs/architecture/webapp.md,
// QRZ sync): which records come in, the fields kept of each, how a record is cut into the slices the main board
// takes one at a time, and a whole import against the relay. webapp/tests/test_qrz_import_core.js runs this
// file under node.
//
// Only FT8 and FT4 QSOs come in: the station works no other mode. A record with no date cannot take its place
// in the list, so it is left out and counted. Each slice names QRZ's ID, so the main board finds the record a
// later slice belongs to, and sending a record again changes nothing; one of our own QSOs that QRZ holds takes
// only QRZ's ID, its confirmation, its region and a grid it lacks (qso_log_store_import()).
//
// Plain global, like qrz_sync_core.js: no module system.
var QrzImport = (function () {
  'use strict';
  var SLICE_MAX = 254;    // PORTA_LOG_IMPORT_FIELDS_MAX, porta_proto.h
  var BODY_MAX = 3000;    // of LOG_IMPORT_BODY_MAX (3,072), sidekick/main/host_link.c, a request's lines
  var MAX_PAGES = 40;     // of 250 records: where a fetch gives up, 10,000 records
  var FROM = '1980-01-01';
  // The fields kept, in the order they go: the call first, since the first slice makes the record. Never a
  // name, address or email (the relay passes none of them either).
  var FIELDS = ['call', 'qso_date', 'time_on', 'band', 'mode', 'submode', 'freq', 'station_callsign', 'gridsquare',
    'rst_sent', 'rst_rcvd', 'app_qrzlog_status', 'app_qrzlog_qsldate', 'dxcc', 'cont', 'state', 'cnty', 'cqz', 'ituz'];

  function upper(s) { return String(s == null ? '' : s).toUpperCase().trim(); }

  // 'FT8', 'FT4' (QRZ writes it as a mode, or as MFSK with SUBMODE FT4, as the sync's match() reads it), or ''.
  function modeOf(r) {
    var m = upper(r.mode), sub = upper(r.submode);
    if (m === 'FT8') return 'FT8';
    if (m === 'FT4' || (m === 'MFSK' && sub === 'FT4')) return 'FT4';
    return '';
  }

  // Why a record stays out: 'mode', 'date' or 'call'; '' when it comes in.
  function leftOut(r) {
    if (!modeOf(r)) return 'mode';
    if (!/^\d{8}$/.test(String(r.qso_date || ''))) return 'date';
    if (!upper(r.call) || !Number(r.app_qrzlog_logid)) return 'call';
    return '';
  }

  function field(name, value) {
    value = String(value == null ? '' : value).replace(/[\r\n<>]+/g, ' ').trim();
    return value ? '<' + name + ':' + value.length + '>' + value : '';
  }

  // The record's fields as the station keeps them: FT4 as MODE MFSK with SUBMODE FT4, as it logs its own.
  function fieldsOf(r) {
    var mode = modeOf(r);
    var vals = {};
    FIELDS.forEach(function (n) { vals[n] = r[n]; });
    vals.call = upper(r.call);
    vals.mode = mode === 'FT4' ? 'MFSK' : 'FT8';
    vals.submode = mode === 'FT4' ? 'FT4' : '';
    return FIELDS.map(function (n) { return field(n, vals[n]); }).filter(function (f) { return f; });
  }

  // The slices of a record, each its QRZ ID and as many of its fields as fit SLICE_MAX bytes, the call in the
  // first. A field that does not fit a slice on its own is left out.
  function slices(r) {
    var id = field('app_qrzlog_logid', String(Number(r.app_qrzlog_logid)));
    var out = [], cur = id;
    fieldsOf(r).forEach(function (f) {
      if (id.length + f.length > SLICE_MAX) return;
      if (cur.length + f.length > SLICE_MAX) { out.push(cur); cur = id; }
      cur += f;
    });
    if (cur !== id || !out.length) out.push(cur);
    return out;
  }

  // Lines into requests of at most BODY_MAX bytes, a record's slices kept in one request where they fit.
  function batches(records) {
    var out = [], cur = [], size = 0;
    records.forEach(function (rec) {
      var bytes = rec.lines.reduce(function (n, l) { return n + l.length + 1; }, 0);
      if (cur.length && size + bytes > BODY_MAX) { out.push(cur); cur = []; size = 0; }
      cur.push(rec);
      size += bytes;
    });
    if (cur.length) out.push(cur);
    return out;
  }

  // One import. Everything it touches is handed in:
  //   key, version    as QrzSync.run() takes them
  //   relay(path, body)  -> Promise of the relay's JSON reply
  //   post(lines)     -> Promise of the station's answer to POST /api/log/import: { taken, refused: [{line, why}] }
  //   today           'YYYY-MM-DD', UTC: the end of the range asked of QRZ (a day on, for a clock behind)
  //   maxCalls        relay calls it may make
  //   progress(done, total)  records sent so far, of those that come in
  // Resolves to { ok, error, reason, calls, fetched, imported, leftOut: {mode, date, call}, refused: [{call, why}] }.
  function run(deps) {
    var out = { ok: false, error: '', reason: '', calls: 0, fetched: 0, imported: 0, leftOut: { mode: 0, date: 0, call: 0 }, refused: [] };
    var maxCalls = deps.maxCalls === undefined ? 30 : deps.maxCalls;
    function call(path, body) {
      if (out.calls >= maxCalls) return Promise.reject({ budget: true });
      out.calls += 1;
      body.key = deps.key;
      if (deps.version) body.app = deps.version;
      return deps.relay(path, body).then(function (reply) {
        if (!reply || reply.ok !== true) throw { reply: reply || {} };
        return reply;
      });
    }
    var to = new Date(Date.parse(deps.today + 'T00:00:00Z') + 86400000).toISOString().slice(0, 10);
    var theirs = [];
    function page(after, n) {
      return call('/qrz/fetch', { from: FROM, to: to, after: after }).then(function (reply) {
        theirs = theirs.concat(reply.records || []);
        if (!reply.more) return;
        if (n + 1 >= MAX_PAGES) throw { reply: { error: 'too_many', reason: '' } };
        return page(reply.next, n + 1);
      });
    }
    return page(0, 0).then(function () {
      out.fetched = theirs.length;
      var recs = [];
      theirs.forEach(function (r) {
        var why = leftOut(r);
        if (why) out.leftOut[why] += 1;
        else recs.push({ call: upper(r.call), lines: slices(r) });
      });
      var done = 0;
      return batches(recs).reduce(function (p, batch) {
        return p.then(function () {
          var lines = [], owner = [];
          batch.forEach(function (rec, k) { rec.lines.forEach(function (l) { lines.push(l); owner.push(k); }); });
          return deps.post(lines).then(function (ans) {
            var bad = {};
            (ans.refused || []).forEach(function (x) { if (bad[owner[x.line]] === undefined) bad[owner[x.line]] = x.why; });
            batch.forEach(function (rec, k) {
              if (bad[k] !== undefined) out.refused.push({ call: rec.call, why: bad[k] });
              else out.imported += 1;
            });
            done += batch.length;
            if (deps.progress) deps.progress(done, recs.length);
          }, function () { throw { station: true }; });
        });
      }, Promise.resolve());
    }).then(function () {
      out.ok = true;
      return out;
    }, function (e) {
      if (e && e.budget) out.error = 'budget';
      else if (e && e.station) out.error = 'station';
      else if (e && e.reply) { out.error = e.reply.error || 'qrz'; out.reason = e.reply.reason || ''; }
      else out.error = 'unreachable';
      return out;
    });
  }

  return { SLICE_MAX: SLICE_MAX, BODY_MAX: BODY_MAX, modeOf: modeOf, leftOut: leftOut, fieldsOf: fieldsOf, slices: slices,
           batches: batches, run: run };
})();
