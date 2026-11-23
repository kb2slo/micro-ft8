// Reporting what the station hears to PSKReporter (docs/architecture/webapp.md, Spotting): the spots
// this page has seen and not yet posted, and when to post them to the site's /pskr/report route
// (webapp/pskr_report.js). webapp/tests/test_pskr_report_core.js runs this file under node.
//
// Every page open on the station posts what it saw. The route's gate makes their posts one report and
// says when the station's next message may go (`retry_in`); a page whose post was too soon keeps its
// spots and posts again then, so a spot waits at most one five-minute window, whichever page saw it.
// A page that sleeps or closes loses what it held: nothing is kept on the station or in the browser.
//
// Plain global, like qrz_sync_core.js: no module system.
var PskrReport = (function () {
  'use strict';

  var MAX_HELD = 200;               // spots one post carries: pskr_report.js, MAX_SPOTS
  var MAX_AGE_S = 55 * 60;          // PSKReporter drops a report older than about an hour
  var MIN_GAP_MS = 60 * 1000;       // between two posts of one page, whatever it is told
  var WINDOW_MS = 5 * 60 * 1000;    // the station's window (pskr_report.js, WINDOW_MS): after a post with
                                    // nothing new, the next waits one, as after a send
  var FIRST_MS = 30 * 1000;         // the most a first post waits, spread so pages do not post together
  var JITTER_MS = 20 * 1000;        // added to each wait, for the same reason
  var BACKOFF_MS = [1, 2, 4, 8, 15].map(function (m) { return m * 60 * 1000; });
  var GRID_RE = /^[A-R]{2}[0-9]{2}$/;

  // A decode event (/api/events, `d` 1) as a spot, or null. `sender(text)` is the call that sent the
  // message (app.html's extractSenderCall()). The locator is the message's last word when it is one
  // and not a sign-off: RR73 is shaped like a locator and is never one here. A decode with no time
  // (the main board's clock unset) or no dial frequency is not a report.
  function spotOf(e, sender) {
    if (!e || !e.x || !e.t || !e.fq) return null;
    var c = sender(e.x);
    if (!c) return null;
    var words = String(e.x).trim().toUpperCase().split(/\s+/);
    var last = words[words.length - 1];
    var g = words.length >= 3 && GRID_RE.test(last) && last !== 'RR73' ? last : '';
    return { c: c, g: g, f: Math.round(e.fq + (e.hz || 0)), s: Math.round(e.snr || 0),
             m: (e.sl | 0) === 75 ? 'FT4' : 'FT8', t: e.t | 0 };
  }

  // deps:
  //   now()             -> ms
  //   random()          -> [0, 1)
  //   post(body)        -> Promise of the route's JSON reply; rejects when unreachable
  //   online()          -> false while the browser knows it has no connection (navigator.onLine); optional.
  //                        On the sidekick's own network it is true with no internet behind it, so the
  //                        back-off is what keeps attempts rare there.
  function create(deps) {
    var held = {};          // sender call -> its latest spot
    var nextAt = 0;         // ms before which this page does not post; 0: post soon after a first spot
    var lastPostAt = 0;
    var inFlight = false;
    var failures = 0;
    var offline = false;
    var last = null;        // { at, sent, error } of the last post that was answered, for the status line

    function jitter(ms) { return Math.floor(deps.random() * ms); }

    function note(spot) {
      if (!spot) return;
      var had = held[spot.c];
      if (!had || had.t <= spot.t) held[spot.c] = spot;
      if (!nextAt) nextAt = deps.now() + jitter(FIRST_MS);
    }

    function prune(nowMs) {
      var oldest = Math.floor(nowMs / 1000) - MAX_AGE_S;
      Object.keys(held).forEach(function (c) { if (held[c].t < oldest) delete held[c]; });
    }

    function heldSpots() {
      return Object.keys(held).map(function (c) { return held[c]; })
        .sort(function (a, b) { return b.t - a.t; }).slice(0, MAX_HELD);
    }

    // Forgets what a post carried, unless a later spot of the same sender came in while it was out.
    function drop(posted) {
      posted.forEach(function (s) { if (held[s.c] === s) delete held[s.c]; });
    }

    // station: { on, call, grid, app }. Posts when it is time and there is something to post;
    // returns the post's promise, or null when it does not post.
    function tick(station) {
      var now = deps.now();
      if (!station || !station.on || !station.call || !station.grid) {
        held = {};
        nextAt = 0;
        return null;
      }
      prune(now);
      if (deps.online && !deps.online()) {
        // No connection: no attempt, and the spots wait.
        offline = true;
        last = { at: now, sent: 0, error: 'offline' };
        return null;
      }
      if (offline) {
        // Back: post now rather than wait out a back-off counted while there was nothing to reach.
        offline = false;
        failures = 0;
        if (nextAt > now) nextAt = now;
      }
      var spots = heldSpots();
      if (inFlight || !spots.length || !nextAt || now < nextAt || now - lastPostAt < MIN_GAP_MS) return null;
      inFlight = true;
      lastPostAt = now;
      var body = { call: station.call, grid: station.grid, app: station.app || '', spots: spots };
      return Promise.resolve().then(function () { return deps.post(body); }).then(function (reply) {
        inFlight = false;
        failures = 0;
        var after = deps.now();
        reply = reply || {};
        var retry = Number(reply.retry_in) || 0;
        if (reply.ok && !reply.sent && retry > 0) {
          // Too soon: keep them, and post when the window opens.
          nextAt = after + retry + jitter(JITTER_MS);
        } else {
          // Sent, already reported, refused or lost on the way to PSKReporter: none of it is posted again.
          // Already reported names no wait, and posting again a minute later would cost a request a minute
          // on a band where everyone heard was reported within the hour: it waits a window, as a send does.
          drop(spots);
          var nothingNew = reply.ok && !reply.sent && retry === 0;
          nextAt = after + Math.max(nothingNew ? WINDOW_MS : retry, MIN_GAP_MS) + jitter(JITTER_MS);
        }
        last = { at: after, sent: reply.ok ? (reply.sent | 0) : 0, wait: !!(reply.ok && !reply.sent && retry > 0),
                 error: reply.ok ? '' : String(reply.error || 'refused') };
        return reply;
      }, function () {
        // The site did not answer: keep them, and back off.
        inFlight = false;
        nextAt = deps.now() + BACKOFF_MS[Math.min(failures, BACKOFF_MS.length - 1)] + jitter(JITTER_MS);
        failures++;
        last = { at: deps.now(), sent: 0, error: 'no_site' };
        return null;
      });
    }

    return {
      note: note, tick: tick,
      held: function () { return heldSpots().length; },
      last: function () { return last; },
      nextAt: function () { return nextAt; }
    };
  }

  // What Settings › Station says under the switch.
  function statusLine(station, last, held, nowMs) {
    if (!station || !station.on) return '';
    if (!station.call || !station.grid) return 'Needs the station\'s call and grid.';
    if (!last) return held ? held + ' waiting to send.' : 'Nothing heard yet.';
    var mins = Math.max(0, Math.round((nowMs - last.at) / 60000));
    var ago = mins < 1 ? 'just now' : mins + ' min ago';
    if (last.error === 'offline') return 'Not sent: the phone is offline.' + (held ? ' ' + held + ' waiting.' : '');
    if (last.error === 'no_site') return 'Not sent: no internet, or the site did not answer, ' + ago + '. Trying again.';
    if (last.error === 'unreachable') return 'PSKReporter did not answer, ' + ago + '.';
    if (last.error) return 'Refused (' + last.error + '), ' + ago + '.';
    if (last.wait) return held + ' waiting for the next report.';
    return last.sent ? 'Sent ' + last.sent + ' ' + (last.sent === 1 ? 'spot' : 'spots') + ', ' + ago + '.'
                     : 'Up to date, ' + ago + '.';
  }

  return {
    MAX_HELD: MAX_HELD, MAX_AGE_S: MAX_AGE_S, MIN_GAP_MS: MIN_GAP_MS, WINDOW_MS: WINDOW_MS, FIRST_MS: FIRST_MS, BACKOFF_MS: BACKOFF_MS,
    spotOf: spotOf, create: create, statusLine: statusLine
  };
})();
