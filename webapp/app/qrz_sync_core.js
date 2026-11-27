// Syncing the station's log with the operator's QRZ logbook, without the page
// (docs/architecture/webapp.md, QRZ sync): which of the station's QSOs QRZ already holds, which to send,
// and how often the relay may be called. webapp/tests/test_qrz_sync_core.js runs this file under node.
//
// QRZ is read before anything is written to it. An insert there cannot be taken back by us, and QRZ's
// own duplicate check is not documented, so a QSO goes up only when no record of QRZ's matches it here.
// What a run learns is written back to the station's record (markFields()): QRZ's ID, its confirmation and
// the upload mark, so the next run reads only the records QRZ still lacks, from any browser. A run cut
// short between QRZ taking a QSO and the station marking it is put right by the next, which matches it.
//
// Plain global, like swr_core.js: no module system.
var QrzSync = (function () {
  'use strict';

  // A QSO and a QRZ record are the same contact when call, band and mode agree and their times are within
  // this many minutes, nearest first, each used once. QRZ keeps time to the minute (measured 2026-11-19),
  // and a record entered there by hand may be some minutes off; LoTW matches within 30.
  var WINDOW_MIN = 30;
  // A QSO dated before this was logged with the clock unset. It is neither matched nor sent.
  var FIRST_YEAR = 2000;
  var BATCH = 10;        // records one insert request carries: worker.js, QRZ_MAX_INSERTS, which says why
  var MAX_PAGES = 40;    // of 250 records: where a pull gives up, 10,000 records in the days asked for

  // The relay is ours and its free plan has a daily limit (docs/architecture/system.md, Third-party
  // services), so the app spends calls on it deliberately: a run on its own waits MIN_GAP_MS after the
  // last, any run waits out BACKOFF_MS after a failure, and an hour sees at most HOUR_CALLS calls.
  var MIN_GAP_MS = 5 * 60 * 1000;
  var HOUR_MS = 60 * 60 * 1000;
  var HOUR_CALLS = 30;
  var BACKOFF_MS = [1, 2, 4, 8, 16, 32, 60].map(function (m) { return m * 60 * 1000; });

  function upper(s) { return String(s == null ? '' : s).toUpperCase().trim(); }

  // Minutes since 1970 for "20261119" and "2154", "21:54" or "215400"; null when either is not a real
  // date and time, or the year is before FIRST_YEAR.
  function minuteOf(date, time) {
    var d = /^(\d{4})(\d{2})(\d{2})$/.exec(String(date || ''));
    var t = /^(\d{2}):?(\d{2})/.exec(String(time || ''));
    if (!d || !t) return null;
    var y = +d[1], mo = +d[2], day = +d[3], h = +t[1], mi = +t[2];
    if (y < FIRST_YEAR || mo < 1 || mo > 12 || day < 1 || day > 31 || h > 23 || mi > 59) return null;
    var ms = Date.UTC(y, mo - 1, day, h, mi);
    return new Date(ms).getUTCDate() === day ? ms / 60000 : null;
  }

  function contactKey(call, band, mode) { return upper(call) + '|' + upper(band) + '|' + upper(mode); }

  // The station's rows against QRZ's records for the same days.
  //   ours:   [{ date: 'YYYYMMDD', time, call, band, mode, station_callsign }], as the log API gives a row,
  //           with its day file's date
  //   theirs: the relay's records, QRZ's fields under QRZ's names: { call, qso_date, time_on, band, mode,
  //           submode, app_qrzlog_logid, ... }. QRZ writes FT4 as a mode; a record that reached it as MFSK
  //           with SUBMODE FT4 is the same contact.
  // Returns indexes into both:
  //   pairs    [{ ours, theirs }]  QSOs QRZ holds
  //   unsent   [ours]              QSOs to send
  //   skipped  [{ ours, why }]     'callsign': logged under a call that is not this logbook's, which is
  //                                another logbook at QRZ; 'date': no usable date
  //   extra    [theirs]            QRZ's records with no QSO here, from another radio or logger
  function match(ours, theirs, logbookCall) {
    var out = { pairs: [], unsent: [], skipped: [], extra: [] };
    var book = upper(logbookCall);
    var mine = [];           // { i, key, minute }
    ours.forEach(function (q, i) {
      var minute = minuteOf(q.date, q.time);
      if (upper(q.station_callsign) !== book) out.skipped.push({ ours: i, why: 'callsign' });
      else if (minute === null) out.skipped.push({ ours: i, why: 'date' });
      else mine.push({ i: i, key: contactKey(q.call, q.band, q.mode), minute: minute });
    });
    var byKey = {};
    var taken = {};
    theirs.forEach(function (r, j) {
      var minute = minuteOf(r.qso_date, r.time_on);
      if (minute === null) return;
      var key = contactKey(r.call, r.band, r.submode || r.mode);
      (byKey[key] = byKey[key] || []).push({ j: j, minute: minute });
    });
    // Every pairing within the window, nearest first, each side used once. Ties go to the lower
    // indexes, so the same inputs always pair the same way.
    var candidates = [];
    mine.forEach(function (m) {
      (byKey[m.key] || []).forEach(function (r) {
        var gap = Math.abs(r.minute - m.minute);
        if (gap <= WINDOW_MIN) candidates.push({ i: m.i, j: r.j, gap: gap });
      });
    });
    candidates.sort(function (a, b) { return a.gap - b.gap || a.i - b.i || a.j - b.j; });
    var paired = {};
    candidates.forEach(function (c) {
      if (paired[c.i] !== undefined || taken[c.j]) return;
      paired[c.i] = c.j;
      taken[c.j] = true;
    });
    mine.forEach(function (m) {
      if (paired[m.i] !== undefined) out.pairs.push({ ours: m.i, theirs: paired[m.i] });
      else out.unsent.push(m.i);
    });
    theirs.forEach(function (r, j) { if (!taken[j]) out.extra.push(j); });
    return out;
  }

  function isoDay(ms) { return new Date(ms).toISOString().slice(0, 10); }

  // The days to ask QRZ for: from the day before the first usable QSO to the day after the last, since a
  // record a few minutes off can fall on the other side of midnight. null when no row is usable.
  function range(ours, logbookCall) {
    var book = upper(logbookCall);
    var lo = null, hi = null;
    ours.forEach(function (q) {
      var minute = minuteOf(q.date, q.time);
      if (minute === null || upper(q.station_callsign) !== book) return;
      if (lo === null || minute < lo) lo = minute;
      if (hi === null || minute > hi) hi = minute;
    });
    if (lo === null) return null;
    var dayMs = 24 * 60 * 60 * 1000;
    return { from: isoDay(lo * 60000 - dayMs), to: isoDay(hi * 60000 + dayMs) };
  }

  function adifField(name, value) {
    value = String(value == null ? '' : value);
    return value ? '<' + name + ':' + value.length + '>' + value : '';
  }
  // The fields a run writes back to a record QRZ holds: QRZ's ID when it named one, its confirmation when
  // it said, and the upload mark, which alone says QRZ holds it when there is no ID (a duplicate).
  //   info: { logid, status, qsldate }, any of them empty; today 'YYYYMMDD'
  function markFields(info, today) {
    info = info || {};
    var id = String(info.logid || '').replace(/\D/g, '');
    var qsl = /^\d{8}$/.test(String(info.qsldate || '')) ? info.qsldate : '';
    return adifField('app_qrzlog_logid', id === '0' ? '' : id) + adifField('app_qrzlog_status', upper(info.status)) +
      adifField('app_qrzlog_qsldate', qsl) + adifField('qrzcom_qso_upload_status', 'Y') +
      adifField('qrzcom_qso_upload_date', /^\d{8}$/.test(String(today || '')) ? today : '');
  }

  function newBudget() { return { lastRunMs: 0, fails: 0, hourStartMs: 0, hourCalls: 0 }; }

  // Whether a run may start now. `asked` is true for one the operator asked for, which does not wait out
  // MIN_GAP_MS; nothing skips the wait after a failure or the hour's limit.
  function mayRun(budget, nowMs, asked) {
    var b = budget || newBudget();
    var waits = [];
    if (b.fails > 0) waits.push({ why: 'failed', until: b.lastRunMs + BACKOFF_MS[Math.min(b.fails, BACKOFF_MS.length) - 1] });
    if (!asked) waits.push({ why: 'recent', until: b.lastRunMs + MIN_GAP_MS });
    if (nowMs - b.hourStartMs < HOUR_MS && b.hourCalls >= HOUR_CALLS) waits.push({ why: 'hour', until: b.hourStartMs + HOUR_MS });
    var worst = null;
    waits.forEach(function (w) { if (w.until > nowMs && (!worst || w.until > worst.until)) worst = w; });
    return worst ? { ok: false, why: worst.why, waitMs: worst.until - nowMs } : { ok: true, why: '', waitMs: 0 };
  }

  // The budget after a run that made `calls` relay calls and ended well or not.
  function noteRun(budget, nowMs, calls, ok) {
    var b = budget || newBudget();
    var fresh = nowMs - b.hourStartMs >= HOUR_MS;
    return {
      lastRunMs: nowMs,
      fails: ok ? 0 : b.fails + 1,
      hourStartMs: fresh ? nowMs : b.hourStartMs,
      hourCalls: (fresh ? 0 : b.hourCalls) + calls,
    };
  }

  // The relay calls a run may still make this hour.
  function callsLeft(budget, nowMs) {
    var b = budget || newBudget();
    return nowMs - b.hourStartMs >= HOUR_MS ? HOUR_CALLS : Math.max(0, HOUR_CALLS - b.hourCalls);
  }

  // One sync. Everything it touches is handed in, so the page, a test and a bench script run the same
  // steps:
  //   key            the logbook's API key
  //   logbook        the logbook's callsign when it is already known, which saves the status call
  //   version        the web app's version, which the relay names to QRZ
  //   relay(path, body)  -> Promise of the relay's JSON reply (worker.js, /qrz/*); rejects when unreachable
  //   readRows()     -> Promise of the station's records QRZ lacks (its unsent ones), each a log row with
  //                     its `id` and `date`; rejects when the station does not answer
  //   mark(row, fields) -> Promise, resolved once the station has set `fields` (markFields()) on the
  //                     row's record; rejects when it has not
  //   today          'YYYYMMDD', UTC: the upload date written back
  //   buildAdif(row) -> the ADIF record to send for a row
  //   maxCalls       relay calls this run may make (callsLeft())
  //   send           false to stop once the plan is known and send nothing
  //   sendFrom       'YYYYMMDD': with send, only QSOs dated that day or later go up, and older ones wait
  // Resolves to { ok, error, reason, calls, logbook, rows, held, extra, skipped, plan, sent, duplicates,
  // refused, marked }. error is the relay's ('auth', 'qrz', ...), or 'unreachable', 'station', 'budget' or
  // 'too_many':
  //   held     rows QRZ already held, each marked on the station with QRZ's ID
  //   plan     the rows QRZ lacked; sent, duplicates, refused: what became of those that went
  //   unsent   how many of them QRZ still lacks: not sent, held back by sendFrom, or refused
  //   marked   records the station has marked this run
  function run(deps) {
    var out = { ok: false, error: '', reason: '', calls: 0, logbook: '', rows: 0, held: 0, extra: 0, skipped: [],
      plan: [], sent: 0, duplicates: 0, refused: [], unsent: 0, marked: 0 };
    var atQrz = [];  // by place in the plan: the rows this run put there
    var maxCalls = deps.maxCalls === undefined ? HOUR_CALLS : deps.maxCalls;
    var rows = [];
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
    function fail(e) {
      if (e && e.budget) out.error = 'budget';
      else if (e && e.station) out.error = 'station';
      else if (e && e.reply) { out.error = e.reply.error || 'qrz'; out.reason = e.reply.reason || ''; }
      else out.error = 'unreachable';
      return out;
    }
    function mark(q, info) {
      return deps.mark(q, markFields(info, deps.today)).then(function () { out.marked += 1; },
        function () { throw { station: true }; });
    }
    function markEach(list) {  // [{ row, info }], one after another: each is a write on the main board
      return list.reduce(function (p, m) { return p.then(function () { return mark(m.row, m.info); }); }, Promise.resolve());
    }
    // The station is read first: with nothing unsent, QRZ is not asked anything.
    return deps.readRows().then(null, function () { throw { station: true }; }).then(function (read) {
      rows = read || [];
      out.rows = rows.length;
      if (rows.length === 0) return { callsign: deps.logbook || '' };
      return deps.logbook ? { callsign: deps.logbook } : call('/qrz/status', {});
    }).then(function (status) {
      out.logbook = upper(status.callsign);
      var days = range(rows, out.logbook);
      if (!days) return [];
      var theirs = [];
      function page(after, n) {
        return call('/qrz/fetch', { from: days.from, to: days.to, after: after }).then(function (reply) {
          theirs = theirs.concat(reply.records || []);
          if (!reply.more) return theirs;
          if (n + 1 >= MAX_PAGES) throw { reply: { error: 'too_many', reason: '' } };
          return page(reply.next, n + 1);
        });
      }
      return page(0, 0);
    }).then(function (theirs) {
      var m = match(rows, theirs, out.logbook);
      out.held = m.pairs.length;
      out.extra = m.extra.length;
      out.skipped = m.skipped.map(function (s) { return { row: rows[s.ours], why: s.why }; });
      out.plan = m.unsent.map(function (i) { return rows[i]; });
      return markEach(m.pairs.map(function (pr) {
        var r = theirs[pr.theirs];
        return { row: rows[pr.ours], info: { logid: r.app_qrzlog_logid, status: r.app_qrzlog_status, qsldate: r.app_qrzlog_qsldate } };
      }));
    }).then(function () {
      if (deps.send === false) return;
      var left = [];  // places in the plan
      out.plan.forEach(function (q, k) { if (!deps.sendFrom || q.date >= deps.sendFrom) left.push(k); });
      // What QRZ took is marked on the station before the next batch goes: a duplicate too, which QRZ
      // holds but names no ID for.
      function tally(results, places) {
        var marks = [];
        (results || []).forEach(function (r, k) {
          if (r.result === 'inserted') out.sent += 1;
          else if (r.result === 'duplicate') out.duplicates += 1;
          else out.refused.push({ row: out.plan[places[k]], reason: r.reason || '' });
          if (r.result === 'inserted' || r.result === 'duplicate') {
            atQrz[places[k]] = true;
            marks.push({ row: out.plan[places[k]], info: { logid: r.result === 'inserted' ? r.logid : '' } });
          }
        });
        return markEach(marks);
      }
      function batch() {
        if (left.length === 0) return;
        var now = left.splice(0, BATCH);
        return call('/qrz/insert', { records: now.map(function (k) { return deps.buildAdif(out.plan[k]); }) }).then(function (reply) {
          return tally(reply.results, now).then(batch);
        }, function (e) {
          // A batch the relay stopped part way (a refused key) still says what went before it.
          if (!(e && e.reply)) throw e;
          return tally(e.reply.results, now).then(function () { throw e; });
        });
      }
      return batch();
    }).then(function () {
      out.ok = true;
      out.plan.forEach(function (q, k) { if (!atQrz[k]) out.unsent += 1; });
      return out;
    }, fail);
  }

  return {
    WINDOW_MIN: WINDOW_MIN, FIRST_YEAR: FIRST_YEAR, BATCH: BATCH, MAX_PAGES: MAX_PAGES,
    MIN_GAP_MS: MIN_GAP_MS, HOUR_CALLS: HOUR_CALLS, BACKOFF_MS: BACKOFF_MS,
    minuteOf: minuteOf, match: match, range: range, markFields: markFields,
    newBudget: newBudget, mayRun: mayRun, noteRun: noteRun, callsLeft: callsLeft, run: run
  };
})();
